#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

// Eclipse NULL check
#ifndef NULL
#define NULL   ((void *) 0)
#endif

#include "redismi.h"
#include <zend_exceptions.h>

// ZLIB definition
#ifndef MAX_WBITS
#define MAX_WBITS 15
#endif

/*
 * Class entry pointer itself
 */
zend_class_entry *redismi_ce;

/*
 * Our exceptions
 */
zend_class_entry *redismi_exception_ce;
zend_class_entry *spl_rte_ce = NULL;

/*
 * RedisMI function table
 */
static zend_function_entry redismi_methods[] = {
    PHP_ME(RedisMI, __construct, NULL, ZEND_ACC_PUBLIC)
    PHP_ME(RedisMI, __destruct, NULL, ZEND_ACC_PUBLIC)
    PHP_ME(RedisMI, SetCompression, NULL, ZEND_ACC_PUBLIC)
    PHP_ME(RedisMI, GetInfo, NULL, ZEND_ACC_PUBLIC)
    PHP_ME(RedisMI, SetInfo, NULL, ZEND_ACC_PUBLIC)
    PHP_ME(RedisMI, GetBuffer, NULL, ZEND_ACC_PUBLIC)
    PHP_ME(RedisMI, SetBuffer, NULL, ZEND_ACC_PUBLIC)
    PHP_ME(RedisMI, LoadBuffer, NULL, ZEND_ACC_PUBLIC)
    PHP_ME(RedisMI, SaveBuffer, NULL, ZEND_ACC_PUBLIC)
    PHP_ME(RedisMI, SendBuffer, NULL, ZEND_ACC_PUBLIC)
    PHP_ME(RedisMI, BufferLen, NULL, ZEND_ACC_PUBLIC)
    PHP_ME(RedisMI, CmdCount, NULL, ZEND_ACC_PUBLIC)
    PHP_ME(RedisMI, SaveCallback, NULL, ZEND_ACC_PUBLIC)
    PHP_ME(RedisMI, LastReplyCount, NULL, ZEND_ACC_PUBLIC)
    PHP_ME(RedisMI, LastErrorCount, NULL, ZEND_ACC_PUBLIC)
    PHP_ME(RedisMI, truncate, NULL, ZEND_ACC_PUBLIC)

    /* Pass-thru methods */
    #include "pt_struct.h"

    {NULL, NULL, NULL}
};

/* Pass-thru method implementation */
#include "pt_impl.h"

/*
 * Our own exceptions
 */
PHPAPI zend_class_entry *redismi_get_exception_base(int root TSRMLS_DC) {
#if HAVE_SPL
    if(!root) {
        if(!spl_rte_ce) {
            zend_class_entry **pce;

            if(zend_hash_find(CG(class_table),
                              "runtimeexception", sizeof("runtimeexception"),
                              (void**)&pce) == SUCCESS)
            {
                spl_rte_ce = *pce;
                return *pce;
            }
        } else {
            return spl_rte_ce;
        }
    }
#endif
#if (PHP_MAJOR_VERSION == 5) && (PHP_MINOR_VERSION < 2) 
    return zend_exception_get_default();
#else
    return zend_exception_get_default(TSRMLS_C);
#endif
}

/*
 * Create context structure
 */
zend_object_value create_redismi_context(zend_class_entry *class_type TSRMLS_DC) {
    zend_object_value retval;
    redismi_context *context;

    // Allocate the actual struct
    context = (redismi_context*)emalloc(sizeof(redismi_context));
    memset(context, 0, sizeof(redismi_context));

    zend_object_std_init(&context->std, class_type TSRMLS_CC);

#if PHP_VERSION_ID < 50399
    zval *tmp;

    zend_hash_copy(context->std.properties,
        &class_type->default_properties,
        (copy_ctor_func_t) zval_add_ref,
        (void*)&tmp,
        sizeof(zval *)
    );
#else
    object_properties_init(&(context->std), class_type);
#endif

    retval.handle = zend_objects_store_put(
        context, 
        (zend_objects_store_dtor_t)zend_objects_destroy_object,
        free_redismi_context,
        NULL TSRMLS_CC
    );

    retval.handlers = zend_get_std_object_handlers();

    return retval;
}

/* 
 * Destroy object context
 */
void free_redismi_context(void *object TSRMLS_DC) {
    // Grab context
    redismi_context *context = (redismi_context*)object;

    // Free our command buffer
    if(context->buf) {
        cb_free(context->buf);
    }

    // If we've got a context zval attached to our struct, free it
    if(context->z_info) {
        zval_dtor(context->z_info);
        efree(context->z_info);
    }

    // If we've got an attached callback, free our handler struct
    if(context->fci) {
        /*
         * TODO:  There is a memory leak involved here somehow
         * efreeing the fci.function_name/fci.object_ptr fixes
         * the warning, but that seems the incorrect solution.
         * It must be something to do with ADDREF_P/DELREF_P
        if(context->fci->fci.function_name) {
            efree(context->fci->fci.function_name);
            Z_DELREF_P(context->fci->fci.function_name);
        }
        if(context->fci->fci.object_ptr) {
            efree(context->fci->fci.object_ptr);
            Z_DELREF_P(context->fci->fci.object_ptr);
        }
        */

        efree(context->fci);
    }

    // Free context structure itself
    efree(context);
}

/*
 * Initialize our class and register it with PHP
 */
void init_redismi(TSRMLS_D) {
    zend_class_entry ce, ce_ex;
    
    // Initialize our class entry
    INIT_CLASS_ENTRY(ce, "RedisMI", redismi_methods);

    // Set our function pointer to our context creation function
    ce.create_object = create_redismi_context;

    // Register the class with PHP
    redismi_ce = zend_register_internal_class(&ce TSRMLS_CC);

    // Initialize the exception class entry
    INIT_CLASS_ENTRY(ce_ex, "RedisMIException", NULL);
    redismi_exception_ce = zend_register_internal_class_ex(
        &ce_ex,
        redismi_get_exception_base(0 TSRMLS_CC),
        NULL TSRMLS_CC
    );
}

/*
 * Append a command to our buffer using variable argument counts
 */
PHPAPI void redis_cmd(INTERNAL_FUNCTION_PARAMETERS, char *cmd, size_t cmd_len) {
    zval **z_args;
    int argc = ZEND_NUM_ARGS(), i;
    redismi_context *context = GET_CONTEXT();

    if(!context->buf) {
        zend_throw_exception(redismi_exception_ce, "Invalid command buffer", 0 TSRMLS_CC);
        RETURN_FALSE;
    }

    z_args = emalloc(argc * sizeof(zval*));
    if(zend_get_parameters_array(ht, argc, z_args) == FAILURE) {
        efree(z_args);
        RETURN_FALSE;
    }

    // Command header
    cb_put_redis_hdr(context->buf, cmd, cmd_len, argc);

    // Iterate our arguments
    for(i = 0; i<argc; i++) {
        convert_to_string(z_args[i]);
        cb_put_redis_cmdl(context->buf, Z_STRVAL_P(z_args[i]), Z_STRLEN_P(z_args[i]));
    }

    // Increase the number of commands we've got in the buffer
    context->cmd_count++;

    // Free our argument array
    efree(z_args);

    // Success
    RETURN_TRUE;
}

/*
 * Set object's context value
 */
PHPAPI void set_object_info(INTERNAL_FUNCTION_PARAMETERS, redismi_context *context, zval *z_info) {
    // Free previous context if it's set
    if(context->z_info) {
        zval_dtor(context->z_info);
        efree(context->z_info);
        context->z_info = NULL;
    }

    // If the info is non null, copy it and set.
    if(z_info) {
        MAKE_STD_ZVAL(context->z_info);
        *context->z_info = *z_info;
        zval_copy_ctor(context->z_info);
    }
}

/*
 * Call our attached callback
 */
PHPAPI int exec_save_callback(INTERNAL_FUNCTION_PARAMETERS, redismi_context *context,
                              char *file, int file_len, int cmd_count)
{
    zend_fcall_info        *fci = &context->fci->fci;
    zend_fcall_info_cache  *fcc = &context->fci->fcc;
    zval                   **params[3], *z_ret=NULL, *z_file, *z_cmds;

    // RedisMI
    params[0] = &getThis();

    // Save file
    MAKE_STD_ZVAL(z_file);
    ZVAL_STRINGL(z_file, file, file_len, 0);
    params[1] = &z_file;

    // Number of commands
    MAKE_STD_ZVAL(z_cmds);
    ZVAL_LONG(z_cmds, cmd_count);
    params[2] = &z_cmds;

    // Set up call structure
    fci->retval_ptr_ptr = &z_ret;
    fci->params         = params;
    fci->param_count    = 3;
   	fci->no_separation 	= 0;

   	// Call our function
   	int result = zend_call_function(fci, fcc TSRMLS_CC) != SUCCESS;

   	// Free our arguments
   	efree(z_file);
   	efree(z_cmds);

   	// Free any return value the userland callback decided to create
   	if(z_ret) zval_ptr_dtor(&z_ret);

   	// Success!
   	return result;
}

/*
 * Parse our status line
 */
int parse_status_line(redismi_context *ctx, const char *line) {
    char *p, *p2, buf[40];

    // Grab relevant error pointers
    if(!(p=strchr(line, ' ')) || !(p2=strchr(p,','))) {
        return FAILURE;
    }

    // Copy in error count
    strncpy(buf,p+1,p2-p-1);
    buf[p2-p-1]='\0';
    ctx->send_err_count = atoi(buf);

    // Grab reply pointers
    if(!(p=strchr(p2,':'))) {
        return FAILURE;
    }

    // Copy in reply count
    strcpy(buf,++p);
    ctx->send_cmd_count = atoi(buf);

    // Success
    return SUCCESS;
}

/*
 * Truncate and optionally free buffer memory
 */
int truncate_buffer(redismi_context *ctx, int free) {
    // Reset position and command count
    ctx->buf->pos  = 0;
    ctx->cmd_count = 0;

    // Free and realloc if we're directed to
    if(free) {
        cb_free(ctx->buf);

        // Attempt to create a new one
        if(!(ctx->buf = cb_init(INITIAL_BUFFER_SIZE))) {
            return FAILURE;
        }
    }

    // Sucess
    return SUCCESS;
}

/*
 * Method implementation
 */


PHP_METHOD(RedisMI, __construct) {
    // Initial buffer size
    zval *z_context = NULL;

    // See if we've been given a different initial size
    if(zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "|z", &z_context) == FAILURE) {
        RETURN_FALSE;
    }

    // Grab our context structure
    redismi_context *context = GET_CONTEXT();
    
    // If the user provided context, make a copy and attach it to our class
    if(z_context) {
        set_object_info(INTERNAL_FUNCTION_PARAM_PASSTHRU, context, z_context);
    }

    // Create our command buffer
    context->buf = cb_init(INITIAL_BUFFER_SIZE);
    
    // Something is very wrong if we can't do that
    if(!context->buf) {
        zend_throw_exception(redismi_exception_ce, "Failed to create context buffer.  Possible OOM error", 0 TSRMLS_CC);
    }
    
    // Success
    RETURN_TRUE;
}

PHP_METHOD(RedisMI, __destruct) {
    // Success
    RETURN_TRUE;
}

/*
 * Set or unset compression
 */
PHP_METHOD(RedisMI, SetCompression) {
    redismi_context *context = GET_CONTEXT();
    zend_bool flag;

    // Grab our parameters
    if(zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "b", &flag)
                             == FAILURE)
    {
        RETURN_FALSE;
    }

    // If the user is trying to enable buffer compression, we'll need zlib
    if(flag && !zend_hash_exists(&module_registry, "zlib", sizeof("zlib"))) {
        php_error_docref(NULL TSRMLS_CC, E_ERROR,
                         "Can't enable compression without the zlib library!");
        RETURN_FALSE;
    }

    // Update our context
    context->compression = flag != 0;

    // Success!
    RETURN_TRUE;
}

/*
 * Get user assigned object info
 */
PHP_METHOD(RedisMI, GetInfo) {
    // Grab object context
    redismi_context *context = GET_CONTEXT();

    // If the context is non-null, return it
    if(context->z_info != NULL) {
        RETURN_ZVAL(context->z_info, 1, 0);
    } else {
        RETURN_NULL();
    }
}

/*
 * Set user assigned object info
 */
PHP_METHOD(RedisMI, SetInfo) {
    zval *z_info=NULL;

    // Make sure we can parse parameters
    if(zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "z", &z_info) == FAILURE) {
        RETURN_FALSE;
    }

    redismi_context *context = GET_CONTEXT();

    // Udpate our object info
    set_object_info(INTERNAL_FUNCTION_PARAM_PASSTHRU, context, z_info);

    RETURN_TRUE;
}

/*
 * Set a callback for when we've finished saving a buffer
 */
PHP_METHOD(RedisMI, SaveCallback) {
    // Allocate memory for our fcall info and cache
    callback_handler *cfi = emalloc(sizeof(*cfi));

    if(zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "f", &cfi->fci, &cfi->fcc) == FAILURE) {
        efree(cfi);
        RETURN_FALSE;
    }

    if(cfi->fci.function_name) {
        Z_ADDREF_P(cfi->fci.function_name);
    }
    if(cfi->fci.object_ptr) {
        Z_ADDREF_P(cfi->fci.object_ptr);
    }

    // Grab our object context
    redismi_context *context = GET_CONTEXT();

    // Free any old callback we have
    if(context->fci) efree(context->fci);

    // Set our callback info in our context struct
    context->fci = cfi;

    // Success
    RETURN_TRUE;
}

/*
 * Truncate our command buffer (doesn't free memory)
 */
PHP_METHOD(RedisMI, truncate) {
    int b_free = 0;

    // Make sure we can parse parameters
    if(zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "|b", &b_free) == FAILURE) {
        RETURN_FALSE;
    }

    // Get the classes context object
    redismi_context *context = GET_CONTEXT();

    // If we've been instructed to free memory associated with our command buffer, do that
    if(truncate_buffer(context, b_free) < 0) {
        // Something is wrong
        zend_throw_exception(redismi_exception_ce, "Couldn't reallocate command buffer!", 0 TSRMLS_C);
        RETURN_FALSE;
    }

    // Success!
    RETURN_TRUE;
}

/*
 * Load a buffer file from disk
 */
PHP_METHOD(RedisMI, LoadBuffer) {
    char *filename, *data;
    int filename_len=0, len;
    php_stream *stream;
    php_stream_filter *filter = NULL;
    redismi_context *context = GET_CONTEXT();

    if(zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "p", &filename,
                             &filename_len) == FAILURE) {
        RETURN_FALSE;
    }

    // We can't open an empty file
    if(!filename_len) {
        RETURN_FALSE;
    }

    // Open our buffer file
    if(!(stream = php_stream_open_wrapper_ex(filename, "rb",
                                             0 | REPORT_ERRORS, NULL, NULL)))
    {
        php_error_docref(NULL TSRMLS_CC, E_ERROR, "Couldn't open buffer file '%s'", filename);
        RETURN_FALSE;
    }

    // If this is a gzipped file, we'll want to decompress
    if(IS_GZIPPED_FILE(filename, filename_len)) {
        // This zlib commpressed file should have the gzip header
        zval filterparams;
        array_init(&filterparams);
        add_assoc_long(&filterparams,"window",MAX_WBITS+32);

        // Attempt to create our filter
        filter = php_stream_filter_create("zlib.inflate", &filterparams, 0 TSRMLS_CC);

        // Free parameters
        zval_dtor(&filterparams);

        // If we couldn't create the filter, it could be because ZLIB is not installed
        if(!filter) {
            php_error_docref(NULL TSRMLS_CC, E_ERROR, "Couldn't create filter to decompress buffer file");
            RETURN_FALSE;
        }

        // Add this filter to our stream
        php_stream_filter_append_ex(&stream->readfilters, filter);
    }

    if((len = php_stream_copy_to_mem(stream, &data, -1, PHP_STREAM_COPY_ALL)) < 0) {
        php_error_docref(NULL TSRMLS_CC, E_ERROR, "Couldn't read from stream '%s'", filename);
        RETURN_FALSE
    }

    // Truncate our buffer
    truncate_buffer(context, 0);

    if(cb_appendl(context->buf, data, len) < 0) {
        RETVAL_FALSE;
    } else {
        RETVAL_LONG(len);
    }

    // If we applied a filter, flush and remove it
    if(filter) {
        php_stream_filter_flush(filter, 1 TSRMLS_CC);
        php_stream_filter_remove(filter, 1 TSRMLS_CC);
    }

    // Close our stream
    php_stream_close(stream);
}

/*
 * Save the command buffer to a file
 */
PHP_METHOD(RedisMI, SaveBuffer) {
    char *filename, save_file[1024];
    int filename_len, truncate = 1, written, save_file_len;
    php_stream *stream;
    php_stream_filter *filter = NULL;
    redismi_context *context = GET_CONTEXT();
    int cmd_count = context->cmd_count;

    if(zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "s|b", &filename,
                             &filename_len, &truncate) == FAILURE) {
        RETURN_FALSE;
    }

    // There is no need to save an empty command buffer
    if(!cmd_count) {
        RETURN_FALSE;
    }

    // If we're compressing, we want to append a .gz extension to the buffer filename
    if(context->compression) {
        // Append our gzip extension
        save_file_len = snprintf(save_file, sizeof(save_file), "%s.gz", filename);
    } else {
        // The filename, as is
        save_file_len = snprintf(save_file, sizeof(save_file), "%s", filename);
    }

    // Create our php stream for writing to disk
    stream = php_stream_open_wrapper_ex(save_file, "wb", 0 | REPORT_ERRORS, NULL, NULL);

    if(stream == NULL) {
        RETURN_FALSE;
    }

    // Create a filter for our stream if we're compressing
    if(context->compression) {
        // This bit tells zlib to add the gzip header, so gunzip
        // will be able to decompress it
        zval filterparams;
        array_init(&filterparams);
        add_assoc_long(&filterparams, "window", MAX_WBITS+16);

        // Create our write filter, and instruct zlib to add the header so gunzip can inflate it
        filter = php_stream_filter_create("zlib.deflate", &filterparams, 0 TSRMLS_CC);
        zval_dtor(&filterparams);

        // Abort if we couldn't create the filter
        if(!filter) {
            php_error_docref(NULL TSRMLS_CC, E_ERROR,"Couldn't create filter to compress buffer file!");
            RETURN_FALSE;
        }

        // Append our filter to the write stream
        php_stream_filter_append(&stream->writefilters, filter);
    }

    if((written = php_stream_write(stream, context->buf->buf, context->buf->pos)) < context->buf->pos) {
        php_error_docref(NULL TSRMLS_CC, E_WARNING,
                         "Could only write %d of %d bytes, possibly out of disk space.",
                         written, (int)context->buf->pos);
        written = -1;
    }

    // If we've used a compression filter, free it
    if(filter) {
        php_stream_filter_flush(filter, 1 TSRMLS_CC);
        php_stream_filter_remove(filter, 1 TSRMLS_CC);
    }

    // Close our stream
    php_stream_close(stream);

    // Truncate our buffer
    if(truncate) truncate_buffer(context, 0);

    if(written < 0) {
        RETURN_FALSE;
    }

    // Execute our save callback
    if(context->fci) {
        exec_save_callback(INTERNAL_FUNCTION_PARAM_PASSTHRU, context,
                           save_file, save_file_len, cmd_count);
    }

    // Return the actual filename written which might have appended a .gz
    // extension if compression is enabled
    RETURN_STRINGL(save_file, save_file_len, 1);
}

/*
 * Send a buffer
 */
PHP_METHOD(RedisMI, SendBuffer) {
    char *host, *file, cmd[1024], rbuf[1024];
    long host_len=0, port=0, file_len=0;
    redismi_context *context = GET_CONTEXT();
    FILE *fp;

    // Parse args
    if(zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "ssl", &file, &file_len,
                             &host, &host_len, &port) == FAILURE)
    {
        RETURN_FALSE;
    }

    // We can't do anything if there isn't a file, host, or the port is out of range
    if(!file_len || !host_len || port < 0 || port > 65536) {
        RETURN_FALSE;
    }

    // Construct our command.  If the file has a .gz suffix, we'll use gunzip to
    // decompress it to STDOUT, otherwise just cat the file into redis-cli
    if(context->compression || IS_GZIPPED_FILE(file,file_len)) {
        snprintf(cmd, sizeof(cmd), "gunzip -c %s|redis-cli -h %s -p %d --pipe", file, host, port);
    } else {
        // Construct our redis-cli --pipe command
        snprintf(cmd, sizeof(cmd), "cat %s|redis-cli -h %s -p %d --pipe", file, host, port);
    }

    // Attempt to open the process to read from it
    if((fp = popen(cmd, "r")) == NULL) {
        zend_throw_exception(redismi_exception_ce, "Can't invoke redis-cli to send data!", 0 TSRMLS_CC);
        RETURN_FALSE;
    }

    // Read each response from redis-cli, we just need the last one
    while(fgets(rbuf, sizeof(rbuf), fp) != NULL);

    // Parse our errors: N, replies: N into our context
    parse_status_line(context, rbuf);

    // Close the process
    int rval = pclose(fp);

    // Return true if we have no errors, or an execution error
    if(rval == 0) {
        RETURN_TRUE;
    } else {
        RETURN_FALSE;
    }
}

/**
 * The length of our buffer in bytes
 */
PHP_METHOD(RedisMI, BufferLen) {
    redismi_context *ctx = GET_CONTEXT();
    RETURN_LONG(ctx->buf->size);
}

/**
 * The number of commands in our buffer
 */
PHP_METHOD(RedisMI, CmdCount) {
    redismi_context *ctx = GET_CONTEXT();
    RETURN_LONG(ctx->cmd_count);
}

/*
 * Last reply count
 */
PHP_METHOD(RedisMI, LastReplyCount) {
    redismi_context *ctx = GET_CONTEXT();
    RETURN_LONG(ctx->send_cmd_count);
}

/*
 * Last error count
 */
PHP_METHOD(RedisMI, LastErrorCount) {
    redismi_context *ctx = GET_CONTEXT();
    RETURN_LONG(ctx->send_err_count);
}

/*
 * Get the current command buffer as a string
 */
PHP_METHOD(RedisMI, GetBuffer) {
    redismi_context *context = GET_CONTEXT();
    RETURN_STRINGL(context->buf->buf, context->buf->pos, 1);
}

/*
 * Set the buffer manually
 */
PHP_METHOD(RedisMI, SetBuffer) {
    char *data;
    int data_len;
    redismi_context *context = GET_CONTEXT();

    if(zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "s", &data, &data_len) == FAILURE) {
        RETURN_FALSE;
    }

    context->buf->pos = 0;
    if(cb_appendl(context->buf, data, data_len) < 0) {
        RETURN_FALSE;
    }

    RETURN_TRUE;
}
