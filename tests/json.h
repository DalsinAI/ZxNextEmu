/* A minimal JSON reader for the SingleStepTests runners (tests/sst*.c). */
#ifndef TESTS_JSON_H
#define TESTS_JSON_H

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

typedef enum { J_NULL, J_NUM, J_STR, J_ARR, J_OBJ } jtype;

typedef struct jval {
    jtype t;
    long num;
    char *str;           /* J_STR, or key when inside an object */
    struct jval *kids;   /* J_ARR / J_OBJ children */
    int n, cap;
    char *key;
} jval;

static const char *jp;

static void jskip(void)
{
    while (*jp && isspace((unsigned char)*jp))
        jp++;
}

static void jparse(jval *v);

static jval *jpush(jval *v)
{
    if (v->n == v->cap) {
        v->cap = v->cap ? v->cap * 2 : 8;
        v->kids = realloc(v->kids, sizeof(jval) * (size_t)v->cap);
    }
    memset(&v->kids[v->n], 0, sizeof(jval));
    return &v->kids[v->n++];
}

static char *jstring(void)
{
    const char *s = ++jp;
    char *r;
    while (*jp && *jp != '"')
        jp++;
    r = malloc((size_t)(jp - s) + 1);
    memcpy(r, s, (size_t)(jp - s));
    r[jp - s] = 0;
    jp++;
    return r;
}

static void jparse(jval *v)
{
    jskip();
    if (*jp == '{') {
        v->t = J_OBJ;
        jp++;
        for (;;) {
            jval *k;
            jskip();
            if (*jp == '}') { jp++; break; }
            if (*jp == ',') { jp++; continue; }
            k = jpush(v);
            k->key = jstring();
            jskip();
            jp++; /* : */
            jparse(k);
        }
    } else if (*jp == '[') {
        v->t = J_ARR;
        jp++;
        for (;;) {
            jskip();
            if (*jp == ']') { jp++; break; }
            if (*jp == ',') { jp++; continue; }
            jparse(jpush(v));
        }
    } else if (*jp == '"') {
        v->t = J_STR;
        v->str = jstring();
    } else if (*jp == 'n') {
        v->t = J_NULL;
        jp += 4;
    } else {
        v->t = J_NUM;
        v->num = strtol(jp, (char **)&jp, 10);
    }
}

static void jfree(jval *v)
{
    int i;
    for (i = 0; i < v->n; i++)
        jfree(&v->kids[i]);
    free(v->kids);
    free(v->str);
    free(v->key);
}

static jval *jget(jval *o, const char *key)
{
    int i;
    for (i = 0; i < o->n; i++)
        if (!strcmp(o->kids[i].key, key))
            return &o->kids[i];
    return NULL;
}

static long jnum(jval *o, const char *key)
{
    jval *v = jget(o, key);
    return v ? v->num : 0;
}

#endif
