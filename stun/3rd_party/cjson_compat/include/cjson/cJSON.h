#ifndef CJSON_COMPAT_CJSON_H
#define CJSON_COMPAT_CJSON_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct cJSON {
    struct cJSON *next;
    struct cJSON *prev;
    struct cJSON *child;

    int type;
    char *valuestring;
    int valueint;
    double valuedouble;

    char *string;
} cJSON;

#define cJSON_False 0
#define cJSON_True 1
#define cJSON_NULL 2
#define cJSON_Number 3
#define cJSON_String 4
#define cJSON_Array 5
#define cJSON_Object 6

cJSON *cJSON_CreateArray(void);
cJSON *cJSON_CreateObject(void);
cJSON *cJSON_CreateString(const char *string);
cJSON *cJSON_CreateNumber(double num);

void cJSON_Delete(cJSON *item);

void cJSON_AddItemToArray(cJSON *array, cJSON *item);
void cJSON_AddItemToObject(cJSON *object, const char *name, cJSON *item);

cJSON *cJSON_AddStringToObject(cJSON *object, const char *name, const char *string);
cJSON *cJSON_AddNumberToObject(cJSON *object, const char *name, double number);

#ifdef __cplusplus
}
#endif

#endif
