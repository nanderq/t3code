#pragma once

#include "json.h"

#define CATALOG_PROVIDERS 12
#define CATALOG_MODELS 96

typedef struct {
  char id[65];
  char name[48];
} CatalogProvider;

typedef struct {
  char id[128];
  char label[64];
  int provider;
} CatalogModel;

typedef struct {
  CatalogProvider providers[CATALOG_PROVIDERS];
  CatalogModel models[CATALOG_MODELS];
  int provider_count, model_count;
} ModelCatalog;

void models_parse_provider(JsonCur *c, ModelCatalog *catalog);
void models_selection(JBuf *b, const ModelCatalog *catalog, int index);
