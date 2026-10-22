#include "models.h"

#include <string.h>

#include "util.h"

void models_parse_provider(JsonCur *c, ModelCatalog *catalog) {
  if (catalog->provider_count >= CATALOG_PROVIDERS) {
    json_skip(c);
    return;
  }
  CatalogProvider provider = {0};
  char driver[48] = "";
  bool enabled = false;
  int first = catalog->model_count;
  if (!json_obj_begin(c)) return;
  JStr key;
  while (json_obj_next(c, &key)) {
    if (jstr_eq(key, "instanceId")) json_str(c, provider.id, sizeof provider.id);
    else if (jstr_eq(key, "displayName")) json_str(c, provider.name, sizeof provider.name);
    else if (jstr_eq(key, "driver")) json_str(c, driver, sizeof driver);
    else if (jstr_eq(key, "enabled")) enabled = json_bool(c);
    else if (jstr_eq(key, "models") && json_arr_begin(c)) {
      while (json_arr_next(c)) {
        if (catalog->model_count >= CATALOG_MODELS) {
          json_skip(c);
          continue;
        }
        CatalogModel model = {0};
        // An overlong slug must not become a different, truncated model id.
        char id[129] = "";
        if (!json_obj_begin(c)) continue;
        JStr k2;
        while (json_obj_next(c, &k2)) {
          if (jstr_eq(k2, "slug")) json_str(c, id, sizeof id);
          else if (jstr_eq(k2, "name")) json_str(c, model.label, sizeof model.label);
          else json_skip(c);
        }
        if (!id[0] || strlen(id) >= sizeof model.id) continue;
        util_strlcpy(model.id, id, sizeof model.id);
        if (!model.label[0]) util_strlcpy(model.label, id, sizeof model.label);
        model.provider = catalog->provider_count;
        catalog->models[catalog->model_count++] = model;
      }
    } else json_skip(c);
  }
  if (!enabled || !provider.id[0]) {
    catalog->model_count = first;
    return;
  }
  if (!provider.name[0]) util_strlcpy(provider.name, driver[0] ? driver : provider.id, sizeof provider.name);
  catalog->providers[catalog->provider_count++] = provider;
}

void models_selection(JBuf *b, const ModelCatalog *catalog, int index) {
  jb_init(b);
  if (index < 0 || index >= catalog->model_count) return;
  const CatalogModel *model = &catalog->models[index];
  jb_raw(b, "{");
  jb_kv_str(b, "instanceId", catalog->providers[model->provider].id);
  jb_kv_str(b, "model", model->id);
  jb_raw(b, "}");
}
