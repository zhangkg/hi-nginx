extern "C" {
#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>
#include <ngx_md5.h>
}

#include <openssl/ssl.h>
#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/pem.h>
#include <openssl/err.h>
#include <openssl/bio.h>
#include <openssl/md5.h>
#include <openssl/x509v3.h>

#include <vector>
#include <memory>
#include "include/request.hpp"
#include "include/response.hpp"
#include "include/servlet.hpp"



#include "lib/module_class.hpp"
#include "lib/lrucache.hpp"
#include "lib/param.hpp"
#include "lib/redis.hpp"



#include "lib/py_request.hpp"
#include "lib/py_response.hpp"
#include "lib/boost_py.hpp"

#include "lib/php-x/phpx.h"
#include "lib/php-x/phpx_embed.h"
#include "lib/fmt/format.h"

#include "lib/lua.hpp"




#include "lib/MPFDParser-1.1.1/Parser.h"



#define SESSION_ID_NAME "SESSIONID"
#define form_urlencoded_type "application/x-www-form-urlencoded"
#define form_urlencoded_type_len (sizeof(form_urlencoded_type) - 1)
#define TEMP_DIRECTORY "temp"

struct cache_ele_t {
    int status = 200;
    time_t t;
    std::string content_type, content;
};

static std::vector<std::shared_ptr<hi::module_class<hi::servlet>>> PLUGIN;
static std::vector<std::shared_ptr<hi::cache::lru_cache<std::string, cache_ele_t>>> CACHE;
static std::shared_ptr<hi::redis> REDIS;
static std::shared_ptr<hi::boost_py> PYTHON;
static std::shared_ptr<hi::lua> LUA;
static std::shared_ptr<php::VM> PHP;

enum application_t {
    __cpp__, __python__, __lua__, __php__, __unkown__
};

typedef struct {
    ngx_str_t module_path
    , redis_host
    , python_script
    , python_content
    , lua_script
    , lua_content
    , php_script;
    ngx_int_t redis_port
    , module_index
    , cache_expires
    , session_expires
    , cache_index;
    size_t cache_size;
    ngx_flag_t need_headers
    , need_cache
    , need_cookies
    , need_session;
    application_t app_type;
} ngx_http_hi_loc_conf_t;


static ngx_int_t clean_up(ngx_conf_t *cf);
static char *ngx_http_hi_conf_init(ngx_conf_t *cf, ngx_command_t *cmd, void *conf);
static void * ngx_http_hi_create_loc_conf(ngx_conf_t *cf);
static char * ngx_http_hi_merge_loc_conf(ngx_conf_t* cf, void* parent, void* child);


static ngx_int_t ngx_http_hi_handler(ngx_http_request_t *r);
static void ngx_http_hi_body_handler(ngx_http_request_t* r);
static ngx_int_t ngx_http_hi_normal_handler(ngx_http_request_t *r);


static void get_input_headers(ngx_http_request_t* r, std::unordered_map<std::string, std::string>& input_headers);
static void set_output_headers(ngx_http_request_t* r, std::unordered_multimap<std::string, std::string>& output_headers);
static ngx_str_t get_input_body(ngx_http_request_t *r);

static void ngx_http_hi_cpp_handler(ngx_http_hi_loc_conf_t * conf, hi::request& req, hi::response& res);
static void ngx_http_hi_python_handler(ngx_http_hi_loc_conf_t * conf, hi::request& req, hi::response& res);
static void ngx_http_hi_lua_handler(ngx_http_hi_loc_conf_t * conf, hi::request& req, hi::response& res);

static void ngx_http_hi_php_handler(ngx_http_hi_loc_conf_t * conf, hi::request& req, hi::response& res);

static std::string md5(const std::string& str);
static std::string random_string(const std::string& s);
static bool is_dir(const std::string& s);

ngx_command_t ngx_http_hi_commands[] = {
    {
        ngx_string("hi"),
        NGX_HTTP_LOC_CONF | NGX_HTTP_LIF_CONF | NGX_CONF_TAKE1,
        ngx_http_hi_conf_init,
        NGX_HTTP_LOC_CONF_OFFSET,
        offsetof(ngx_http_hi_loc_conf_t, module_path),
        NULL
    },
    {
        ngx_string("hi_cache_size"),
        NGX_HTTP_MAIN_CONF | NGX_HTTP_SRV_CONF | NGX_HTTP_SIF_CONF | NGX_HTTP_LOC_CONF | NGX_HTTP_LIF_CONF | NGX_CONF_TAKE1,
        ngx_conf_set_num_slot,
        NGX_HTTP_LOC_CONF_OFFSET,
        offsetof(ngx_http_hi_loc_conf_t, cache_size),
        NULL
    },
    {
        ngx_string("hi_cache_expires"),
        NGX_HTTP_MAIN_CONF | NGX_HTTP_SRV_CONF | NGX_HTTP_SIF_CONF | NGX_HTTP_LOC_CONF | NGX_HTTP_LIF_CONF | NGX_CONF_TAKE1,
        ngx_conf_set_sec_slot,
        NGX_HTTP_LOC_CONF_OFFSET,
        offsetof(ngx_http_hi_loc_conf_t, cache_expires),
        NULL
    },
    {
        ngx_string("hi_need_headers"),
        NGX_HTTP_MAIN_CONF | NGX_HTTP_SRV_CONF | NGX_HTTP_SIF_CONF | NGX_HTTP_LOC_CONF | NGX_HTTP_LIF_CONF | NGX_CONF_TAKE1,
        ngx_conf_set_flag_slot,
        NGX_HTTP_LOC_CONF_OFFSET,
        offsetof(ngx_http_hi_loc_conf_t, need_headers),
        NULL
    },
    {
        ngx_string("hi_need_cache"),
        NGX_HTTP_MAIN_CONF | NGX_HTTP_SRV_CONF | NGX_HTTP_SIF_CONF | NGX_HTTP_LOC_CONF | NGX_HTTP_LIF_CONF | NGX_CONF_TAKE1,
        ngx_conf_set_flag_slot,
        NGX_HTTP_LOC_CONF_OFFSET,
        offsetof(ngx_http_hi_loc_conf_t, need_cache),
        NULL
    },
    {
        ngx_string("hi_need_cookies"),
        NGX_HTTP_MAIN_CONF | NGX_HTTP_SRV_CONF | NGX_HTTP_SIF_CONF | NGX_HTTP_LOC_CONF | NGX_HTTP_LIF_CONF | NGX_CONF_TAKE1,
        ngx_conf_set_flag_slot,
        NGX_HTTP_LOC_CONF_OFFSET,
        offsetof(ngx_http_hi_loc_conf_t, need_cookies),
        NULL
    },
    {
        ngx_string("hi_redis_host"),
        NGX_HTTP_MAIN_CONF | NGX_HTTP_SRV_CONF | NGX_HTTP_SIF_CONF | NGX_CONF_TAKE1,
        ngx_conf_set_str_slot,
        NGX_HTTP_LOC_CONF_OFFSET,
        offsetof(ngx_http_hi_loc_conf_t, redis_host),
        NULL
    },
    {
        ngx_string("hi_redis_port"),
        NGX_HTTP_MAIN_CONF | NGX_HTTP_SRV_CONF | NGX_HTTP_SIF_CONF | NGX_CONF_TAKE1,
        ngx_conf_set_num_slot,
        NGX_HTTP_LOC_CONF_OFFSET,
        offsetof(ngx_http_hi_loc_conf_t, redis_port),
        NULL
    },
    {
        ngx_string("hi_need_session"),
        NGX_HTTP_MAIN_CONF | NGX_HTTP_SRV_CONF | NGX_HTTP_SIF_CONF | NGX_HTTP_LOC_CONF | NGX_HTTP_LIF_CONF | NGX_CONF_TAKE1,
        ngx_conf_set_flag_slot,
        NGX_HTTP_LOC_CONF_OFFSET,
        offsetof(ngx_http_hi_loc_conf_t, need_session),
        NULL
    },
    {
        ngx_string("hi_session_expires"),
        NGX_HTTP_MAIN_CONF | NGX_HTTP_SRV_CONF | NGX_HTTP_SIF_CONF | NGX_HTTP_LOC_CONF | NGX_HTTP_LIF_CONF | NGX_CONF_TAKE1,
        ngx_conf_set_sec_slot,
        NGX_HTTP_LOC_CONF_OFFSET,
        offsetof(ngx_http_hi_loc_conf_t, session_expires),
        NULL
    },
    {
        ngx_string("hi_python_script"),
        NGX_HTTP_LOC_CONF | NGX_HTTP_LIF_CONF | NGX_CONF_TAKE1,
        ngx_http_hi_conf_init,
        NGX_HTTP_LOC_CONF_OFFSET,
        offsetof(ngx_http_hi_loc_conf_t, python_script),
        NULL
    },
    {
        ngx_string("hi_python_content"),
        NGX_HTTP_LOC_CONF | NGX_HTTP_LIF_CONF | NGX_CONF_TAKE1,
        ngx_http_hi_conf_init,
        NGX_HTTP_LOC_CONF_OFFSET,
        offsetof(ngx_http_hi_loc_conf_t, python_content),
        NULL
    },
    {
        ngx_string("hi_lua_script"),
        NGX_HTTP_LOC_CONF | NGX_HTTP_LIF_CONF | NGX_CONF_TAKE1,
        ngx_http_hi_conf_init,
        NGX_HTTP_LOC_CONF_OFFSET,
        offsetof(ngx_http_hi_loc_conf_t, lua_script),
        NULL
    },
    {
        ngx_string("hi_lua_content"),
        NGX_HTTP_LOC_CONF | NGX_HTTP_LIF_CONF | NGX_CONF_TAKE1,
        ngx_http_hi_conf_init,
        NGX_HTTP_LOC_CONF_OFFSET,
        offsetof(ngx_http_hi_loc_conf_t, lua_content),
        NULL
    },
    {
        ngx_string("hi_php_script"),
        NGX_HTTP_LOC_CONF | NGX_HTTP_LIF_CONF | NGX_CONF_TAKE1,
        ngx_http_hi_conf_init,
        NGX_HTTP_LOC_CONF_OFFSET,
        offsetof(ngx_http_hi_loc_conf_t, php_script),
        NULL
    },
    ngx_null_command
};


ngx_http_module_t ngx_http_hi_module_ctx = {
    clean_up, /* preconfiguration */
    NULL, /* postconfiguration */
    NULL, /* create main configuration */
    NULL, /* init main configuration */

    NULL, /* create server configuration */
    NULL, /* merge server configuration */

    ngx_http_hi_create_loc_conf, /* create location configuration */
    ngx_http_hi_merge_loc_conf /* merge location configuration */
};




ngx_module_t ngx_http_hi_module = {
    NGX_MODULE_V1,
    &ngx_http_hi_module_ctx, /* module context */
    ngx_http_hi_commands, /* module directives */
    NGX_HTTP_MODULE, /* module type */
    NULL, /* init master */
    NULL, /* init module */
    NULL, /* init process */
    NULL, /* init thread */
    NULL, /* exit thread */
    NULL, /* exit process */
    NULL, /* exit master */
    NGX_MODULE_V1_PADDING
};

static ngx_int_t clean_up(ngx_conf_t *cf) {
    PLUGIN.clear();
    CACHE.clear();
    REDIS.reset();
    PYTHON.reset();
    LUA.reset();
    PHP.reset();
    return NGX_OK;
}

static char *ngx_http_hi_conf_init(ngx_conf_t *cf, ngx_command_t *cmd, void *conf) {
    ngx_http_core_loc_conf_t *clcf;
    clcf = (ngx_http_core_loc_conf_t *) ngx_http_conf_get_module_loc_conf(cf, ngx_http_core_module);
    clcf->handler = ngx_http_hi_handler;
    ngx_conf_set_str_slot(cf, cmd, conf);
    return NGX_CONF_OK;
}

static void * ngx_http_hi_create_loc_conf(ngx_conf_t *cf) {
    ngx_http_hi_loc_conf_t *conf = (ngx_http_hi_loc_conf_t*) ngx_pcalloc(cf->pool, sizeof (ngx_http_hi_loc_conf_t));
    if (conf) {
        conf->module_path.len = 0;
        conf->module_path.data = NULL;
        conf->module_index = NGX_CONF_UNSET;
        conf->redis_host.len = 0;
        conf->redis_host.data = NULL;
        conf->python_script.len = 0;
        conf->python_script.data = NULL;
        conf->python_content.len = 0;
        conf->python_content.data = NULL;
        conf->lua_script.len = 0;
        conf->lua_script.data = NULL;
        conf->lua_content.len = 0;
        conf->lua_content.data = NULL;
        conf->php_script.len = 0;
        conf->php_script.data = NULL;
        conf->redis_port = NGX_CONF_UNSET;
        conf->cache_size = NGX_CONF_UNSET_UINT;
        conf->cache_expires = NGX_CONF_UNSET;
        conf->session_expires = NGX_CONF_UNSET;
        conf->cache_index = NGX_CONF_UNSET;
        conf->need_headers = NGX_CONF_UNSET;
        conf->need_cache = NGX_CONF_UNSET;
        conf->need_cookies = NGX_CONF_UNSET;
        conf->need_session = NGX_CONF_UNSET;
        conf->app_type = application_t::__unkown__;
        return conf;
    }
    return NGX_CONF_ERROR;
}

static char * ngx_http_hi_merge_loc_conf(ngx_conf_t* cf, void* parent, void* child) {
    ngx_http_hi_loc_conf_t * prev = (ngx_http_hi_loc_conf_t*) parent;
    ngx_http_hi_loc_conf_t * conf = (ngx_http_hi_loc_conf_t*) child;

    ngx_conf_merge_str_value(conf->module_path, prev->module_path, "");
    ngx_conf_merge_str_value(conf->redis_host, prev->redis_host, "");
    ngx_conf_merge_str_value(conf->python_script, prev->python_script, "");
    ngx_conf_merge_str_value(conf->python_content, prev->python_content, "");
    ngx_conf_merge_str_value(conf->lua_script, prev->lua_script, "");
    ngx_conf_merge_str_value(conf->lua_content, prev->lua_content, "");
    ngx_conf_merge_str_value(conf->php_script, prev->php_script, "");
    ngx_conf_merge_value(conf->redis_port, prev->redis_port, (ngx_int_t) 0);
    ngx_conf_merge_uint_value(conf->cache_size, prev->cache_size, (size_t) 10);
    ngx_conf_merge_sec_value(conf->cache_expires, prev->cache_expires, (ngx_int_t) 300);
    ngx_conf_merge_sec_value(conf->session_expires, prev->session_expires, (ngx_int_t) 300);
    ngx_conf_merge_value(conf->need_headers, prev->need_headers, (ngx_flag_t) 0);
    ngx_conf_merge_value(conf->need_cache, prev->need_cache, (ngx_flag_t) 1);
    ngx_conf_merge_value(conf->need_cookies, prev->need_cookies, (ngx_flag_t) 0);
    ngx_conf_merge_value(conf->need_session, prev->need_session, (ngx_flag_t) 0);
    if (conf->need_session == 1 && conf->need_cookies == 0) {
        conf->need_cookies = 1;
    }
    if (conf->module_index == NGX_CONF_UNSET && conf->module_path.len > 0) {

        ngx_int_t index = NGX_CONF_UNSET;
        bool found = false;
        for (auto& item : PLUGIN) {
            ++index;
            if (item->get_module() == (char*) conf->module_path.data) {
                found = true;
                break;
            }
        }
        if (found) {
            conf->module_index = index;
        } else {
            PLUGIN.push_back(std::make_shared<hi::module_class < hi::servlet >> ((char*) conf->module_path.data));
            conf->module_index = PLUGIN.size() - 1;
        }
        conf->app_type = application_t::__cpp__;
    }

    if (conf->python_content.len > 0 || conf->python_script.len > 0) {
        conf->app_type = application_t::__python__;
    }
    if (conf->lua_content.len > 0 || conf->lua_script.len > 0) {
        conf->app_type = application_t::__lua__;
    }
    if (conf->php_script.len > 0) {
        conf->app_type = application_t::__php__;
        if (!PHP) {
            int argc = 1;
            char* argv[2] = {"", NULL};
            PHP = std::move(std::make_shared<php::VM>(argc, argv));
        }
    }

    if (conf->need_cache == 1 && conf->cache_index == NGX_CONF_UNSET) {
        CACHE.push_back(std::make_shared<hi::cache::lru_cache < std::string, cache_ele_t >> (conf->cache_size));
        conf->cache_index = CACHE.size() - 1;
    }


    return NGX_CONF_OK;
}

static ngx_int_t ngx_http_hi_handler(ngx_http_request_t *r) {
    if (r->headers_in.content_length_n > 0) {
        ngx_http_core_loc_conf_t *clcf = (ngx_http_core_loc_conf_t *) ngx_http_get_module_loc_conf(r, ngx_http_core_module);
        if (clcf->client_body_buffer_size < (size_t) clcf->client_max_body_size) {
            clcf->client_body_buffer_size = clcf->client_max_body_size;
        }
        r->request_body_in_single_buf = 1;
        r->request_body_file_log_level = 0;
        ngx_int_t rc = ngx_http_read_client_request_body(r, ngx_http_hi_body_handler);
        if (rc >= NGX_HTTP_SPECIAL_RESPONSE) {
            return rc;
        }
        return NGX_DONE;
    } else {
        ngx_http_discard_request_body(r);
        return ngx_http_hi_normal_handler(r);
    }
}

static ngx_int_t ngx_http_hi_normal_handler(ngx_http_request_t *r) {

    ngx_http_hi_loc_conf_t * conf = (ngx_http_hi_loc_conf_t *) ngx_http_get_module_loc_conf(r, ngx_http_hi_module);

    if (r->headers_in.if_modified_since && r->headers_in.if_modified_since->value.data) {
        time_t now = time(NULL), old = ngx_http_parse_time(r->headers_in.if_modified_since->value.data, r->headers_in.if_modified_since->value.len);
        if (difftime(now, old) <= conf->cache_expires) {
            return NGX_HTTP_NOT_MODIFIED;
        }
    }

    hi::request ngx_request;
    hi::response ngx_response;
    std::string SESSION_ID_VALUE;

    ngx_request.uri.assign((char*) r->uri.data, r->uri.len);
    if (r->args.len > 0) {
        ngx_request.param.assign((char*) r->args.data, r->args.len);
    }
    std::shared_ptr<std::string> cache_k;
    if (conf->need_cache == 1) {
        ngx_response.headers.insert(std::make_pair("Last-Modified", (char*) ngx_cached_http_time.data));
        cache_k = std::make_shared<std::string>(ngx_request.uri);
        if (r->args.len > 0) {
            cache_k->append("?").append(ngx_request.param);
        }
        u_char *p;
        ngx_md5_t md5;
        u_char md5_buf[16];

        p = (u_char*) ngx_palloc(r->pool, 32);
        if (p == NULL) {
            return NGX_HTTP_INTERNAL_SERVER_ERROR;
        }

        ngx_md5_init(&md5);
        ngx_md5_update(&md5, (u_char*) cache_k->c_str(), cache_k->size());
        ngx_md5_final(md5_buf, &md5);
        ngx_hex_dump(p, md5_buf, sizeof (md5_buf));

        cache_k->assign((char*) p, 32);

        if (CACHE[conf->cache_index]->exists(*cache_k)) {
            const cache_ele_t& cache_v = CACHE[conf->cache_index]->get(*cache_k);
            time_t now = time(NULL);
            if (difftime(now, cache_v.t) > conf->cache_expires) {
                CACHE[conf->cache_index]->erase(*cache_k);
            } else {
                ngx_response.content = cache_v.content;
                ngx_response.headers.find("Content-Type")->second = cache_v.content_type;
                ngx_response.status = cache_v.status;
                goto done;
            }
        }
    }
    if (conf->need_headers == 1) {
        get_input_headers(r, ngx_request.headers);
    }

    ngx_request.method.assign((char*) r->method_name.data, r->method_name.len);
    ngx_request.client.assign((char*) r->connection->addr_text.data, r->connection->addr_text.len);
    if (r->headers_in.user_agent->value.len > 0) {
        ngx_request.user_agent.assign((char*) r->headers_in.user_agent->value.data, r->headers_in.user_agent->value.len);
    }
    if (r->args.len > 0) {
        hi::parser_param(ngx_request.param, ngx_request.form);
    }
    if (r->headers_in.content_length_n > 0) {
        ngx_str_t body = get_input_body(r);
        if (r->headers_in.content_type->value.len < form_urlencoded_type_len
                || ngx_strncasecmp(r->headers_in.content_type->value.data, (u_char *) form_urlencoded_type,
                form_urlencoded_type_len) != 0) {
            try {
                if ((is_dir(TEMP_DIRECTORY) || mkdir(TEMP_DIRECTORY, S_IRWXU | S_IRWXG | S_IROTH | S_IXOTH) == 0)) {
                    ngx_http_core_loc_conf_t *clcf = (ngx_http_core_loc_conf_t *) ngx_http_get_module_loc_conf(r, ngx_http_core_module);
                    std::shared_ptr<MPFD::Parser> POSTParser(new MPFD::Parser());
                    POSTParser->SetTempDirForFileUpload(TEMP_DIRECTORY);
                    POSTParser->SetUploadedFilesStorage(MPFD::Parser::StoreUploadedFilesInFilesystem);
                    POSTParser->SetMaxCollectedDataLength(clcf->client_max_body_size);
                    POSTParser->SetContentType((char*) r->headers_in.content_type->value.data);
                    POSTParser->AcceptSomeData((char*) body.data, body.len);
                    auto fields = POSTParser->GetFieldsMap();
                    for (auto &item : fields) {
                        if (item.second->GetType() == MPFD::Field::TextType) {
                            ngx_request.form.insert(std::make_pair(item.first, item.second->GetTextTypeContent()));
                        } else {
                            std::string upload_file_name = item.second->GetFileName(), ext;
                            std::string::size_type p = upload_file_name.find_last_of(".");
                            if (p != std::string::npos) {
                                ext = upload_file_name.substr(p);
                            }
                            std::string temp_file = TEMP_DIRECTORY + ("/" + random_string(ngx_request.client + item.second->GetFileName()).append(ext));
                            rename(item.second->GetTempFileName().c_str(), temp_file.c_str());
                            ngx_request.form.insert(std::make_pair(item.first, temp_file));
                        }
                    }
                }
            } catch (MPFD::Exception& err) {
                ngx_response.content = err.GetError();
                ngx_response.status = 500;
                goto done;
            }
        } else {
            hi::parser_param(std::string((char*) body.data, body.len), ngx_request.form);
        }
    }
    if (conf->need_cookies == 1 && r->headers_in.cookies.elts != NULL && r->headers_in.cookies.nelts != 0) {
        ngx_table_elt_t ** cookies = (ngx_table_elt_t **) r->headers_in.cookies.elts;
        for (size_t i = 0; i < r->headers_in.cookies.nelts; ++i) {
            if (cookies[i]->value.data != NULL) {
                hi::parser_param(std::string((char*) cookies[i]->value.data, cookies[i]->value.len), ngx_request.cookies, ';');
            }
        }
    }
    if (conf->need_session == 1 && ngx_request.cookies.find(SESSION_ID_NAME) != ngx_request.cookies.end()) {
        if (!REDIS) {
            REDIS = std::make_shared<hi::redis>();
        }
        if (!REDIS->is_connected() && conf->redis_host.len > 0 && conf->redis_port > 0) {
            REDIS->connect((char*) conf->redis_host.data, (int) conf->redis_port);
        }
        if (REDIS->is_connected()) {
            SESSION_ID_VALUE = ngx_request.cookies[SESSION_ID_NAME ];
            if (!REDIS->exists(SESSION_ID_VALUE)) {
                REDIS->hset(SESSION_ID_VALUE, SESSION_ID_NAME, SESSION_ID_VALUE);
                REDIS->expire(SESSION_ID_VALUE, conf->session_expires);
                ngx_request.session[SESSION_ID_NAME] = SESSION_ID_VALUE;
            } else {
                REDIS->hgetall(SESSION_ID_VALUE, ngx_request.session);
            }
        }
    }
    switch (conf->app_type) {
        case application_t::__cpp__:ngx_http_hi_cpp_handler(conf, ngx_request, ngx_response);
            break;
        case application_t::__python__:ngx_http_hi_python_handler(conf, ngx_request, ngx_response);
            break;
        case application_t::__lua__:ngx_http_hi_lua_handler(conf, ngx_request, ngx_response);
            break;
        case application_t::__php__:ngx_http_hi_php_handler(conf, ngx_request, ngx_response);
            break;
        default:break;
    }

    if (ngx_response.status == 200 && conf->need_cache == 1 && conf->cache_expires > 0) {
        cache_ele_t cache_v;
        cache_v.content = ngx_response.content;
        cache_v.content_type = ngx_response.headers.find("Content-Type")->second;
        cache_v.status = ngx_response.status;
        cache_v.t = time(NULL);
        CACHE[conf->cache_index]->put(*cache_k, cache_v);
    }
    if (REDIS && REDIS->is_connected() && !SESSION_ID_VALUE.empty()) {
        REDIS->hmset(SESSION_ID_VALUE, ngx_response.session);
    }

done:
    ngx_str_t response;
    response.data = (u_char*) ngx_response.content.c_str();
    response.len = ngx_response.content.size();


    ngx_buf_t *buf;
    buf = (ngx_buf_t*) ngx_pcalloc(r->pool, sizeof (ngx_buf_t));
    if (buf == NULL) {
        ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "Failed to allocate response buffer.");
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }

    buf->pos = response.data;
    buf->last = buf->pos + response.len;
    buf->memory = 1;
    buf->last_buf = 1;

    ngx_chain_t out;
    out.buf = buf;
    out.next = NULL;

    set_output_headers(r, ngx_response.headers);
    r->headers_out.status = ngx_response.status;
    r->headers_out.content_length_n = response.len;

    ngx_int_t rc;
    rc = ngx_http_send_header(r);
    if (rc != NGX_OK) {
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }

    return ngx_http_output_filter(r, &out);

}

static void ngx_http_hi_body_handler(ngx_http_request_t* r) {
    ngx_http_finalize_request(r, ngx_http_hi_normal_handler(r));
}

static void get_input_headers(ngx_http_request_t* r, std::unordered_map<std::string, std::string>& input_headers) {
    ngx_table_elt_t *th;
    ngx_list_part_t *part;
    part = &r->headers_in.headers.part;
    th = (ngx_table_elt_t*) part->elts;
    ngx_uint_t i;
    for (i = 0; /* void */; i++) {
        if (i >= part->nelts) {
            if (part->next == NULL) {
                break;
            }
            part = part->next;
            th = (ngx_table_elt_t*) part->elts;
            i = 0;
        }
        input_headers[(char*) th[i].key.data] = std::move(std::string((char*) th[i].value.data, th[i].value.len));
    }
}

static void set_output_headers(ngx_http_request_t* r, std::unordered_multimap<std::string, std::string>& output_headers) {
    for (auto& item : output_headers) {
        ngx_table_elt_t * h = (ngx_table_elt_t *) ngx_list_push(&r->headers_out.headers);
        if (h) {
            h->hash = 1;
            h->key.data = (u_char*) item.first.c_str();
            h->key.len = item.first.size();
            h->value.data = (u_char*) item.second.c_str();
            h->value.len = item.second.size();
        }
    }

}

static ngx_str_t get_input_body(ngx_http_request_t *r) {
    u_char *p;
    u_char *data;
    size_t len;
    ngx_buf_t *buf, *next;
    ngx_chain_t *cl;
    ngx_str_t body = ngx_null_string;

    if (r->request_body == NULL || r->request_body->bufs == NULL) {
        return body;
    }

    if (r->request_body->temp_file) {
        ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "temp_file: %s", r->request_body->temp_file->file.name.data);
        body = r->request_body->temp_file->file.name;
        return body;
    } else {
        cl = r->request_body->bufs;
        buf = cl->buf;

        if (cl->next == NULL) {
            len = buf->last - buf->pos;
            p = (u_char*) ngx_pnalloc(r->pool, len + 1);
            if (p == NULL) {
                return body;
            }
            data = p;
            ngx_memcpy(p, buf->pos, len);
            data[len] = 0;
        } else {
            next = cl->next->buf;
            len = (buf->last - buf->pos) + (next->last - next->pos);
            p = (u_char*) ngx_pnalloc(r->pool, len + 1);
            data = p;
            if (p == NULL) {
                return body;
            }
            p = ngx_cpymem(p, buf->pos, buf->last - buf->pos);
            ngx_memcpy(p, next->pos, next->last - next->pos);
            data[len] = 0;
        }
    }

    body.len = len;
    body.data = data;
    return body;
}

static void ngx_http_hi_cpp_handler(ngx_http_hi_loc_conf_t * conf, hi::request& req, hi::response& res) {
    std::shared_ptr<hi::servlet> view_instance = std::move(PLUGIN[conf->module_index]->make_obj());
    if (view_instance) {
        view_instance->handler(req, res);
    }

}

static void ngx_http_hi_python_handler(ngx_http_hi_loc_conf_t * conf, hi::request& req, hi::response& res) {
    hi::py_request py_req;
    hi::py_response py_res;
    py_req.init(&req);
    py_res.init(&res);
    if (!PYTHON) {
        PYTHON = std::make_shared<hi::boost_py>();
    }
    if (PYTHON) {
        PYTHON->set_req(&py_req);
        PYTHON->set_res(&py_res);
        if (conf->python_script.len > 0) {
            PYTHON->call_script(std::string((char*) conf->python_script.data, conf->python_script.len).append(req.uri));
        } else if (conf->python_content.len > 0) {
            PYTHON->call_content((char*) conf->python_content.data);
        }
    }
}

static void ngx_http_hi_lua_handler(ngx_http_hi_loc_conf_t * conf, hi::request& req, hi::response& res) {
    hi::py_request py_req;
    hi::py_response py_res;
    py_req.init(&req);
    py_res.init(&res);
    if (!LUA) {
        LUA = std::make_shared<hi::lua>();
    }
    if (LUA) {
        LUA->set_req(&py_req);
        LUA->set_res(&py_res);
        if (conf->lua_script.len > 0) {
            LUA->call_script(std::string((char*) conf->lua_script.data, conf->lua_script.len).append(req.uri));
        } else if (conf->lua_content.len > 0) {
            LUA->call_content((char*) conf->lua_content.data);
        }
    }
}

static std::string md5(const std::string& str) {
    unsigned char digest[16] = {0};
    MD5_CTX ctx;
    MD5_Init(&ctx);
    MD5_Update(&ctx, str.c_str(), str.size());
    MD5_Final(digest, &ctx);

    unsigned char tmp[32] = {0}, *dst = &tmp[0], *src = &digest[0];
    unsigned char hex[] = "0123456789abcdef";
    int len = 16;
    while (len--) {
        *dst++ = hex[*src >> 4];
        *dst++ = hex[*src++ & 0xf];
    }

    return std::string((char*) tmp, 32);
}

static std::string random_string(const std::string& s) {
    time_t now = time(NULL);
    char* now_str = ctime(&now);
    return md5(s + now_str);
}

static bool is_dir(const std::string& s) {
    struct stat st;
    return stat(s.c_str(), &st) >= 0 && S_ISDIR(st.st_mode);
}

static void ngx_http_hi_php_handler(ngx_http_hi_loc_conf_t * conf, hi::request& req, hi::response& res) {
    std::string script = std::move(std::string((char*) conf->php_script.data, conf->php_script.len).append(req.uri));
    if (access(script.c_str(), F_OK) == 0) {
        zend_first_try
                {
            PHP->include(script.c_str());
            const char *request = "\\hi\\request", *response = "\\hi\\response", *handler = "handler";
            php::Object php_req = php::newObject(request), php_res = php::newObject(response);
            if (!php_req.isNull() && !php_res.isNull()) {
                php_req.set("client", php::Variant(req.client));
                php_req.set("method", php::Variant(req.method));
                php_req.set("user_agent", php::Variant(req.user_agent));
                php_req.set("param", php::Variant(req.param));
                php_req.set("uri", php::Variant(req.uri));

                php::Array php_req_headers, php_req_form, php_req_cookies, php_req_session;
                for (auto & i : req.headers) {
                    php_req_headers.set(i.first.c_str(), php::Variant(i.second));
                }
                for (auto & i : req.form) {
                    php_req_form.set(i.first.c_str(), php::Variant(i.second));
                }
                for (auto & i : req.cookies) {
                    php_req_cookies.set(i.first.c_str(), php::Variant(i.second));
                }
                for (auto & i : req.session) {
                    php_req_session.set(i.first.c_str(), php::Variant(i.second));
                }
                php_req.set("headers", php_req_headers);
                php_req.set("form", php_req_form);
                php_req.set("cookies", php_req_cookies);
                php_req.set("session", php_req_session);



                auto p = req.uri.find_last_of('/'), q = req.uri.find_last_of('.');

                std::string class_name = std::move(req.uri.substr(p + 1, q - 1 - p));


                php::Object servlet = php::newObject(class_name.c_str());


                if (!servlet.isNull() && servlet.methodExists(handler)) {
                    servlet.exec(handler, php_req, php_res);
                    php::Array res_headers = php_res.get("headers"), res_session = php_res.get("session");


                    for (auto i = res_headers.begin(); i != res_headers.end(); i++) {
                        auto v = i.value();
                        if (v.isArray()) {
                            php::Array arr(v);
                            for (size_t j = 0; j < arr.count(); j++) {
                                res.headers.insert(std::move(std::make_pair(i.key().toString(), arr[j].toString())));
                            }
                        } else {
                            res.headers.insert(std::move(std::make_pair(i.key().toString(), i.value().toString())));
                        }
                    }
                    for (auto i = res_session.begin(); i != res_session.end(); i++) {
                        res.session.insert(std::move(std::make_pair(i.key().toString(), i.value().toString())));
                    }



                    res.content = std::move(php_res.get("content").toString());

                    res.status = std::move(php_res.get("status")).toInt();
                    return;
                }
            }}zend_catch{
            res.content = std::move(fmt::format("<p style='text-align:center;margin:100px;'>{}</p>", "PHP Throw Exception"));
            res.status = 500;}zend_end_try();
    }
}