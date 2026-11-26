/* WUP_V6_JSON_LANGUAGES: English is compiled in; foreign languages are data.
 * No gettext/locale dependency, no GUI rebuilding on selection, and no shell.
 * Missing translations always fall back to the original English UI string. */
#include "i18n.h"
#include <string.h>

typedef struct {
    char *id;
    char *name;
    GHashTable *entries;    /* English source -> translated UTF-8 text */
} wup_language_t;

struct wup_i18n {
    GPtrArray *languages;   /* wup_language_t *; English always at [0] */
    wup_language_t *current;
};

static void free_language(gpointer data) {
    wup_language_t *l = data;
    if (!l) return;
    g_free(l->id);
    g_free(l->name);
    if (l->entries) g_hash_table_unref(l->entries);
    g_free(l);
}

static gboolean valid_id(const char *id) {
    if (!id || !id[0] || strlen(id) > 63) return FALSE;
    for (const unsigned char *p=(const unsigned char *)id; *p; ++p)
        if (!g_ascii_islower(*p) && !g_ascii_isdigit(*p) &&
            *p != '-' && *p != '_') return FALSE;
    return TRUE;
}

static wup_language_t *find_language(const wup_i18n_t *tr, const char *id) {
    if (!tr || !tr->languages || !id) return NULL;
    for (guint i=0; i<tr->languages->len; i++) {
        wup_language_t *l=g_ptr_array_index(tr->languages,i);
        if (g_strcmp0(l->id,id)==0) return l;
    }
    return NULL;
}

static wup_language_t *read_language(const char *path) {
    JsonParser *parser=json_parser_new();
    GError *error=NULL;
    if (!json_parser_load_from_file(parser,path,&error)) {
        g_warning("Language %s: %s",path,error ? error->message : "bad JSON");
        g_clear_error(&error);
        g_object_unref(parser);
        return NULL;
    }
    JsonNode *root=json_parser_get_root(parser);
    JsonObject *object=root && JSON_NODE_HOLDS_OBJECT(root)
                            ? json_node_get_object(root) : NULL;
    const char *id=object ? json_object_get_string_member_with_default(object,"id",NULL) : NULL;
    const char *name=object ? json_object_get_string_member_with_default(object,"name",NULL) : NULL;
    JsonObject *translations=object && json_object_has_member(object,"translations")
                            ? json_object_get_object_member(object,"translations") : NULL;
    if (!valid_id(id) || !g_strcmp0(id,"en") || !name || !g_utf8_validate(name,-1,NULL) ||
        !*name || strlen(name)>100 || !translations) {
        g_warning("Language %s: invalid id/name/translations",path);
        g_object_unref(parser);
        return NULL;
    }
    wup_language_t *lang=g_new0(wup_language_t,1);
    lang->id=g_strdup(id);
    lang->name=g_strdup(name);
    lang->entries=g_hash_table_new_full(g_str_hash,g_str_equal,g_free,g_free);
    GList *keys=json_object_get_members(translations);
    for (GList *it=keys; it; it=it->next) {
        const char *source=it->data;
        JsonNode *node=json_object_get_member(translations,source);
        if (!node || !JSON_NODE_HOLDS_VALUE(node) ||
            json_node_get_value_type(node)!=G_TYPE_STRING) continue;
        const char *value=json_node_get_string(node);
        if (value && *value && g_utf8_validate(source,-1,NULL) &&
            g_utf8_validate(value,-1,NULL))
            g_hash_table_replace(lang->entries,g_strdup(source),g_strdup(value));
    }
    g_list_free(keys);
    g_object_unref(parser);
    return lang;
}

static void scan_language_dir(wup_i18n_t *tr, const char *path) {
    if (!path) return;
    GDir *dir=g_dir_open(path,0,NULL);
    if (!dir) return;
    GList *names=NULL;
    const char *entry;
    while ((entry=g_dir_read_name(dir)))
        if (g_str_has_suffix(entry,".json"))
            names=g_list_prepend(names,g_strdup(entry));
    g_dir_close(dir);
    names=g_list_sort(names,(GCompareFunc)g_strcmp0);
    for (GList *it=names; it; it=it->next) {
        char *full=g_build_filename(path,it->data,NULL);
        wup_language_t *lang=read_language(full);
        g_free(full);
        if (!lang) continue;
        wup_language_t *old=find_language(tr,lang->id);
        if (old) {
            for (guint i=0; i<tr->languages->len; i++)
                if (g_ptr_array_index(tr->languages,i)==old) {
                    g_ptr_array_index(tr->languages,i)=lang;
                    free_language(old);
                    break;
                }
        } else {
            g_ptr_array_add(tr->languages,lang);
        }
    }
    g_list_free_full(names,g_free);
}

void wup_i18n_reload(wup_i18n_t *tr) {
    if (!tr) return;
    char *previous=g_strdup(tr->current ? tr->current->id : "en");
    if (tr->languages) g_ptr_array_unref(tr->languages);
    tr->languages=g_ptr_array_new_with_free_func(free_language);
    wup_language_t *en=g_new0(wup_language_t,1);
    en->id=g_strdup("en"); en->name=g_strdup("English");
    g_ptr_array_add(tr->languages,en);
    tr->current=en;

    /* System < project < user precedence, like the JSON theme loader. */
    const gchar *const *system=g_get_system_data_dirs();
    for (guint i=0; system && system[i]; i++) {
        char *dir=g_build_filename(system[i],"wupstreamd","languages",NULL);
        scan_language_dir(tr,dir); g_free(dir);
    }
    char *exe=g_file_read_link("/proc/self/exe",NULL);
    char *bin=exe ? g_path_get_dirname(exe) : NULL;
    /* WUP_V911_APPIMAGE_ASSETS: prefer embedded AppImage language JSONs when present.
     * Keep custom per-user translations at the highest precedence. */
    char *bundled=bin ? g_build_filename(bin,"..","share","wup-thinserver","languages",NULL) : NULL;
    const char *app_root=g_getenv("APPDIR");
    char *app_data=app_root ? g_build_filename(app_root,"usr","share","wup-thinserver","languages",NULL) : NULL;
    char *dev=bin ? g_build_filename(bin,"..","data","languages",NULL) : NULL;
    scan_language_dir(tr,bundled);
    scan_language_dir(tr,app_data);
    scan_language_dir(tr,dev);
    if (!dev || !g_file_test(dev,G_FILE_TEST_IS_DIR))
        scan_language_dir(tr,"data/languages");
    char *usr=g_build_filename(g_get_user_config_dir(),"wup-thinserver","languages",NULL);
    scan_language_dir(tr,usr);
    g_free(usr);g_free(dev);g_free(app_data);g_free(bundled);g_free(bin);g_free(exe);
    tr->current=find_language(tr,previous);
    if (!tr->current) tr->current=en;
    g_free(previous);
}

wup_i18n_t *wup_i18n_new(const char *requested_id) {
    wup_i18n_t *tr=g_new0(wup_i18n_t,1);
    wup_i18n_reload(tr);
    wup_i18n_select(tr,requested_id);
    return tr;
}

void wup_i18n_free(wup_i18n_t *tr) {
    if (!tr) return;
    if (tr->languages) g_ptr_array_unref(tr->languages);
    g_free(tr);
}
void wup_i18n_select(wup_i18n_t *tr, const char *id) {
    if (!tr) return;
    wup_language_t *found=find_language(tr,id);
    tr->current=found ? found : find_language(tr,"en");
}
const char *wup_i18n_selected(const wup_i18n_t *tr) {
    return tr && tr->current ? tr->current->id : "en";
}
const char *wup_i18n_lookup(const wup_i18n_t *tr, const char *english) {
    if (!english) return "";
    if (!tr || !tr->current || !tr->current->entries) return english;
    const char *value=g_hash_table_lookup(tr->current->entries,english);
    return value && *value ? value : english;
}

