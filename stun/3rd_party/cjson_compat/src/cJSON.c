#include "cjson/cJSON.h"

#include <stdlib.h>
#include <string.h>

static char *cjson_strdup(const char *s)
{
    if (!s) {
        return NULL;
    }
    const size_t len = strlen(s);
    char *out = (char *)malloc(len + 1);
    if (!out) {
        return NULL;
    }
    memcpy(out, s, len + 1);
    return out;
}

static cJSON *cjson_new_item(int type)
{
    cJSON *item = (cJSON *)calloc(1, sizeof(cJSON));
    if (!item) {
        return NULL;
    }
    item->type = type;
    return item;
}

cJSON *cJSON_CreateArray(void)
{
    return cjson_new_item(cJSON_Array);
}

cJSON *cJSON_CreateObject(void)
{
    return cjson_new_item(cJSON_Object);
}

cJSON *cJSON_CreateString(const char *string)
{
    cJSON *item = cjson_new_item(cJSON_String);
    if (!item) {
        return NULL;
    }
    item->valuestring = cjson_strdup(string ? string : "");
    if (!item->valuestring) {
        free(item);
        return NULL;
    }
    return item;
}

cJSON *cJSON_CreateNumber(double num)
{
    cJSON *item = cjson_new_item(cJSON_Number);
    if (!item) {
        return NULL;
    }
    item->valuedouble = num;
    item->valueint = (int)num;
    return item;
}

static void cjson_append_child(cJSON *parent, cJSON *child)
{
    if (!parent || !child) {
        return;
    }

    if (!parent->child) {
        parent->child = child;
        return;
    }

    cJSON *tail = parent->child;
    while (tail->next) {
        tail = tail->next;
    }
    tail->next = child;
    child->prev = tail;
}

void cJSON_AddItemToArray(cJSON *array, cJSON *item)
{
    cjson_append_child(array, item);
}

void cJSON_AddItemToObject(cJSON *object, const char *name, cJSON *item)
{
    if (!object || !item) {
        return;
    }
    item->string = cjson_strdup(name ? name : "");
    if (!item->string) {
        cJSON_Delete(item);
        return;
    }
    cjson_append_child(object, item);
}

cJSON *cJSON_AddStringToObject(cJSON *object, const char *name, const char *string)
{
    cJSON *item = cJSON_CreateString(string);
    if (!item) {
        return NULL;
    }
    cJSON_AddItemToObject(object, name, item);
    if (!item->string) {
        return NULL;
    }
    return item;
}

cJSON *cJSON_AddNumberToObject(cJSON *object, const char *name, double number)
{
    cJSON *item = cJSON_CreateNumber(number);
    if (!item) {
        return NULL;
    }
    cJSON_AddItemToObject(object, name, item);
    if (!item->string) {
        return NULL;
    }
    return item;
}

void cJSON_Delete(cJSON *item)
{
    while (item) {
        cJSON *next = item->next;
        if (item->child) {
            cJSON_Delete(item->child);
        }
        free(item->valuestring);
        free(item->string);
        free(item);
        item = next;
    }
}
