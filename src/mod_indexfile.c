#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>


#include "base.h"
#include "log.h"
#include "buffer.h"

#include "plugin.h"

#include "file_cache_funcs.h"

#include "config.h"

/**
 * this is a indexfile for a lighttpd plugin
 * 
 * just replaces every occurance of 'indexfile' by your plugin name
 * 
 * e.g. in vim:
 * 
 *   :%s/indexfile/myhandler/
 * 
 */



/* plugin config for all request/connections */

typedef struct {
	array *indexfiles;
} plugin_config;

typedef struct {
	PLUGIN_DATA;
	
	buffer *tmp_buf;
	
	plugin_config **config_storage;
	
	plugin_config conf; 
} plugin_data;

/* init the plugin data */
INIT_FUNC(mod_indexfile_init) {
	plugin_data *p;
	
	p = calloc(1, sizeof(*p));
	
	p->tmp_buf = buffer_init();
	
	return p;
}

/* detroy the plugin data */
FREE_FUNC(mod_indexfile_free) {
	plugin_data *p = p_d;
	
	UNUSED(srv);

	if (!p) return HANDLER_GO_ON;
	
	if (p->config_storage) {
		size_t i;
		for (i = 0; i < srv->config_context->used; i++) {
			plugin_config *s = p->config_storage[i];
			
			array_free(s->indexfiles);
			
			free(s);
		}
		free(p->config_storage);
	}
	
	buffer_free(p->tmp_buf);
	
	free(p);
	
	return HANDLER_GO_ON;
}

/* handle plugin config and check values */

SETDEFAULTS_FUNC(mod_indexfile_set_defaults) {
	plugin_data *p = p_d;
	size_t i = 0;
	
	config_values_t cv[] = { 
		{ "index-file.extensions",       NULL, T_CONFIG_ARRAY, T_CONFIG_SCOPE_CONNECTION },       /* 0 */
		{ NULL,                         NULL, T_CONFIG_UNSET, T_CONFIG_SCOPE_UNSET }
	};
	
	if (!p) return HANDLER_ERROR;
	
	p->config_storage = malloc(srv->config_context->used * sizeof(specific_config *));
	
	for (i = 0; i < srv->config_context->used; i++) {
		plugin_config *s;
		
		s = malloc(sizeof(plugin_config));
		s->indexfiles    = array_init();
		
		cv[0].destination = s->indexfiles;
		
		p->config_storage[i] = s;
	
		if (0 != config_insert_values_global(srv, ((data_config *)srv->config_context->data[i])->value, cv)) {
			return HANDLER_ERROR;
		}
	}
	
	return HANDLER_GO_ON;
}

#define PATCH(x) \
	p->conf.x = s->x;
static int mod_indexfile_patch_connection(server *srv, connection *con, plugin_data *p, const char *stage, size_t stage_len) {
	size_t i, j;
	
	/* skip the first, the global context */
	for (i = 1; i < srv->config_context->used; i++) {
		data_config *dc = (data_config *)srv->config_context->data[i];
		plugin_config *s = p->config_storage[i];
		
		/* not our stage */
		if (!buffer_is_equal_string(dc->comp_key, stage, stage_len)) continue;
		
		/* condition didn't match */
		if (!config_check_cond(srv, con, dc)) continue;
		
		/* merge config */
		for (j = 0; j < dc->value->used; j++) {
			data_unset *du = dc->value->data[j];
			
			if (buffer_is_equal_string(du->key, CONST_STR_LEN("index-file.extensions"))) {
				PATCH(indexfiles);
			}
		}
	}
	
	return 0;
}

static int mod_indexfile_setup_connection(server *srv, connection *con, plugin_data *p) {
	plugin_config *s = p->config_storage[0];
	UNUSED(srv);
	UNUSED(con);
		
	PATCH(indexfiles);
	
	return 0;
}
#undef PATCH

URIHANDLER_FUNC(mod_indexfile_subrequest) {
	plugin_data *p = p_d;
	size_t k, i;
	int found;
	
	UNUSED(srv);

	if (con->uri.path->used == 0) return HANDLER_GO_ON;
	if (con->uri.path->ptr[con->uri.path->used - 2] != '/') return HANDLER_GO_ON;
	
	mod_indexfile_setup_connection(srv, con, p);
	for (i = 0; i < srv->config_patches->used; i++) {
		buffer *patch = srv->config_patches->ptr[i];
		
		mod_indexfile_patch_connection(srv, con, p, CONST_BUF_LEN(patch));
	}
	
	found = 0;
	/* indexfile */
	
	/* we will replace it anyway, as physical.path will change */
	file_cache_release_entry(srv, file_cache_get_entry(srv, con->physical.path));
	
	for (k = 0; !found && (k < p->conf.indexfiles->used); k++) {
		data_string *ds = (data_string *)p->conf.indexfiles->data[k];
		file_cache_entry *fce = NULL;
		
		buffer_copy_string_buffer(p->tmp_buf, con->physical.path);
		buffer_append_string_buffer(p->tmp_buf, ds->value);
		
		switch (file_cache_add_entry(srv, con, p->tmp_buf, &(fce))) {
		case HANDLER_GO_ON:
			/* rewrite uri.path to the real path (/ -> /index.php) */
			buffer_append_string_buffer(con->uri.path, ds->value);
			buffer_copy_string_buffer(con->physical.path, p->tmp_buf);
			
			/* fce is already set up a few lines above */
			
			found = 1;
			break;
		case HANDLER_ERROR:
			if (errno == EACCES) {
				con->http_status = 403;
				buffer_reset(con->physical.path);
				
				return HANDLER_FINISHED;
			}
			
			if (errno != ENOENT &&
			    errno != ENOTDIR) {
				/* we have no idea what happend. let's tell the user so. */
				
				con->http_status = 500;
				
				log_error_write(srv, __FILE__, __LINE__, "ssbsb",
						"file not found ... or so: ", strerror(errno),
						con->uri.path,
						"->", con->physical.path);
				
				buffer_reset(con->physical.path);
				
				return HANDLER_FINISHED;
			}
			
			break;
		default:
			break;
		}
	}

	/* not found */
	return HANDLER_GO_ON;
}

/* this function is called at dlopen() time and inits the callbacks */

int mod_indexfile_plugin_init(plugin *p) {
	p->version     = LIGHTTPD_VERSION_ID;
	p->name        = buffer_init_string("indexfile");
	
	p->init        = mod_indexfile_init;
	p->handle_subrequest = mod_indexfile_subrequest;
	p->set_defaults  = mod_indexfile_set_defaults;
	p->cleanup     = mod_indexfile_free;
	
	p->data        = NULL;
	
	return 0;
}