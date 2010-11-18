/* param.c:
 *
 * A parser for a configuration file format with c-like syntax.  Tokens
 * are nested within higher level structures denoted by { or }.  Every
 * token is an array of strings, with simplified syntax for arrays of length
 * one.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <limits.h>
#include <stdarg.h>
#include <ctype.h>
#include <assert.h>
#include <bot_core/lcm_util.h>
#include "param_client.h"
#include "misc_utils.h"
#include <lcmtypes/bot2_param.h>
#include <glib.h>

#define err(args...) fprintf(stderr, args)

#define MAX_REFERENCES ((1LL << 60))

typedef struct _Parser Parser;
typedef struct _ParserFile ParserFile;
typedef struct _ParserString ParserString;
typedef struct _BotParamElement BotParamElement;

typedef int (*GetChFunc)(Parser *);

typedef enum {
  ParserTypeFile, ParserTypeString
} ParserType;

struct _Parser {
  ParserType type;
  GetChFunc get_ch;
  int extra_ch;
};

struct _ParserFile {
  Parser p;
  const char * filename;
  FILE * file;
  int row;
  int col;
  int in_comment;
};

struct _ParserString {
  Parser p;
  const char * string;
  int length;
  int ind;
  int row;
  int col;
  int in_comment;
};

typedef enum {
  TokInvalid,
  TokIdentifier,
  TokOpenStruct,
  TokCloseStruct,
  TokOpenArray,
  TokCloseArray,
  TokArraySep,
  TokAssign,
  TokString,
  TokEndStatement,
  TokCast,
  TokEOF,
} BotParamToken;

typedef enum {
  BotParamContainer, BotParamArray
} BotParamType;

typedef enum {
  BotParamDataString, BotParamDataInt, BotParamDataBool, BotParamDataDouble
} BotParamDataType;

struct _BotParamElement {
  BotParamType type;
  BotParamDataType data_type;
  BotParamElement * parent;
  char * name;
  BotParamElement * next;
  BotParamElement * children;
  int num_values;
  char ** values;
};

struct _BotParam {
  BotParamElement * root;
  GMutex * lock;
  int64_t server_id;
  int64_t sequence_number;
};

/* Prints an error message, preceeded by useful context information from the
 * parser (i.e. line number). */
static int print_msg(Parser * p, char * format, ...)
{
  va_list args;
  if (p->type == ParserTypeFile) {
    ParserFile * pf = (ParserFile *) p;
    const char * fname = strrchr(pf->filename, '/');
    if (fname)
      fname++;
    else
      fname = pf->filename;
    fprintf(stderr, "%s:%d ", fname, pf->row + 1);
    va_start (args, format);
    vfprintf(stderr, format, args);
    va_end (args);
    return 0;
  }
  else if (p->type == ParserTypeFile) {
    ParserString * ps = (ParserString *) p;
    fprintf(stderr, "%s:%d ", "STRING_BUFFER", ps->row + 1);
    va_start (args, format);
    vfprintf(stderr, format, args);
    va_end (args);
    return 0;
  }
  return -1;
}

/* Get the next character from a file, while converting all forms of
 * whitespace into plain spaces and stripping comments.
 *
 * Returns the next printable character on success, 0 on EOF, -1 on
 * error.
 */
static int get_ch_file(Parser * p)
{
  ParserFile * pf = (ParserFile *) p;
  int ch;

  /* If a character has been put back with unget_ch, get it. */
  if (p->extra_ch) {
    ch = p->extra_ch;
    p->extra_ch = 0;
    return ch;
  }

  while ((ch = getc (pf->file)) != EOF) {
    if (ch == '\n') {
      pf->row++;
      pf->col = 0;
      pf->in_comment = 0;
      return ' ';
    }
    if (ch == '#')
      pf->in_comment = 1;
    if (pf->in_comment)
      continue;

    pf->col++;
    if (isspace (ch))
      return ' ';
    if (!isprint (ch)) {
      print_msg(p, "Error: Non-printable character 0x%02x\n", ch);
      return -1;
    }
    return ch;
  }
  return 0;
}

/* Get the next character from the string buffer, while converting all forms of
 * whitespace into plain spaces and stripping comments.
 *
 * Returns the next printable character on success, 0 on EOF, -1 on
 * error.
 */
static int get_ch_string(Parser * p)
{
  ParserString * ps = (ParserString *) p;
  int ch;

  /* If a character has been put back with unget_ch, get it. */
  if (p->extra_ch) {
    ch = p->extra_ch;
    p->extra_ch = 0;
    return ch;
  }

  while (ps->ind < ps->length) {
    ch = ps->string[ps->ind++];
    if (ch == '\n') {
      ps->row++;
      ps->col = 0;
      ps->in_comment = 0;
      return ' ';
    }
    if (ch == '#')
      ps->in_comment = 1;
    if (ps->in_comment)
      continue;

    ps->col++;
    if (isspace (ch))
      return ' ';
    if (!isprint (ch)) {
      print_msg(p, "Error: Non-printable character 0x%02x\n", ch);
      return -1;
    }
    return ch;
  }
  return 0;
}

/* Returns a previously gotten character to the buffer, so it will be gotten
 * next.  This function cannot be used more than once before getting the
 * next character. */
static int unget_ch(Parser * p, int ch)
{
  p->extra_ch = ch;
  return 0;
}

/* Get the next token from the parser.  All information about what constitutes
 * a token is expressed in this function.
 *
 * The type of token is stored in tok.  The actual text of the token is stored
 * in str, a buffer of length len passed by the caller.
 */
static int get_token(Parser * p, BotParamToken * tok, char * str, int len)
{
  int ch, end_ch = 0, c = 0, escape = 0;
  *tok = TokInvalid;

  /* Skip whitespace (all whitespace converted to ' ' already) */
  while ((ch = p->get_ch(p)) == ' ')
    ;

  if (ch == -1)
    return -1;

  if (len < 4) {
    fprintf(stderr, "Error: not enough space to store token\n");
    return -1;
  }

  if (ch == 0) {
    snprintf(str, len, "EOF");
    *tok = TokEOF;
    return 0;
  }

  str[c++] = ch;
  if (ch == ';') {
    *tok = TokEndStatement;
    goto finish;
  }
  if (ch == '=') {
    *tok = TokAssign;
    goto finish;
  }
  if (ch == '[') {
    *tok = TokOpenArray;
    goto finish;
  }
  if (ch == ']') {
    *tok = TokCloseArray;
    goto finish;
  }
  if (ch == '{') {
    *tok = TokOpenStruct;
    goto finish;
  }
  if (ch == '}') {
    *tok = TokCloseStruct;
    goto finish;
  }
  if (ch == ',') {
    *tok = TokArraySep;
    goto finish;
  }

  /* A string always starts with a double quote */
  if (ch == '\"') {
    c--;
    *tok = TokString;
    end_ch = '\"';
    escape = '\\';
  }
  /* A cast always starts with an open paren.
   * TODO: this will need to be tokenized further once the cast is actually
   * used for something. */
  if (ch == '(') {
    c--;
    *tok = TokCast;
    end_ch = ')';
    escape = 0;
  }
  /* An identifier starts with alpha-numeric text or a few symbols */
  if (isalnum (ch) || ch == '_' || ch == '-' || ch == '.') {
    *tok = TokIdentifier;
    end_ch = 0;
    escape = 0;
  }

  if (*tok == TokInvalid) {
    print_msg(p, "Error: Unexpected character \"%c\"\n", ch);
    return -1;
  }

  /* Read the remaining text of a string, cast, or identifier */
  int prev_ch = 0;
  while (1) {
    ch = p->get_ch(p);
    /* An identifier is terminated as soon as we see a character which
     * itself cannot be part of an identifier. */
    if (*tok == TokIdentifier && !isalnum (ch) && ch != '_' && ch != '-' && ch != '.') {
      if (ch != 0)
        unget_ch(p, ch);
      goto finish;
    }
    if (ch == 0) {
      print_msg(p, "Error: Expected '%c' but got end-of-file\n", end_ch);
      return -1;
    }
    /* Strings or casts are terminated when their respective end
     * characters are read, as long as the character is not escaped. */
    if (ch == end_ch && prev_ch != escape)
      goto finish;
    prev_ch = ch;
    str[c++] = ch;

    if (c >= len) {
      print_msg(p, "Error: Token is too large for buffer (%d bytes)\n", len);
      return -1;
    }
  }
  finish: str[c] = '\0';
  return 0;
}

static BotParamElement *
new_element(const char * name)
{
  BotParamElement * el;

  el = malloc(sizeof(BotParamElement));
  memset(el, 0, sizeof(BotParamElement));
  if (name)
    el->name = strdup(name);
  el->data_type = BotParamDataString;

  return el;
}

static void free_element(BotParamElement * el)
{
  free(el->name);
  BotParamElement * child, *next;
  for (child = el->children; child; child = next) {
    next = child->next;
    free_element(child);
  }
  int i;
  for (i = 0; i < el->num_values; i++)
    free(el->values[i]);
  free(el->values);
  free(el);
}

#if 0
/* Debugging function that prints all tokens sequentially from a file */
static int
print_all_tokens (Parser * p)
{
  BotParamToken tok;
  char str[256];

  while (get_token (p, &tok, str, sizeof (str)) == 0) {
    printf ("tok %d: %s\n", tok, str);
    if (tok == TokEOF)
    return 0;
  }
  return -1;
}
#endif

/* Appends child to the list of el's children. */
static int add_child(Parser * p, BotParamElement * el, BotParamElement * child)
{
  BotParamElement ** nptr;
  for (nptr = &el->children; *nptr != NULL; nptr = &((*nptr)->next))
    ;
  *nptr = child;
  child->next = NULL;
  child->parent = el;
  return 0;
}

/* Appends str to the list of el's values. */
static int add_value(Parser * p, BotParamElement * el, const char * str)
{
  int n = el->num_values;
  el->values = realloc(el->values, (n + 1) * sizeof(char *));
  el->values[n] = strdup(str);
  el->num_values = n + 1;
  return 0;
}

/* Parses the interior portion of an array (the part after the leading "["),
 * adding any values to the array's list of values.  Terminates when the
 * trailing "]" is found.
 */
static int parse_array(Parser * p, BotParamElement * el)
{
  BotParamToken tok;
  char str[256];

  while (1) {
    if (get_token(p, &tok, str, sizeof(str)) < 0)
      goto fail;

    if (tok == TokIdentifier || tok == TokString) {
      add_value(p, el, str);
    }
    else if (tok == TokCloseArray) {
      return 0;
    }
    else {
      print_msg(p, "Error: unexpected token \"%s\", expected value or "
        "end of array\n", str);
      goto fail;
    }

    if (get_token(p, &tok, str, sizeof(str)) < 0)
      goto fail;

    if (tok == TokArraySep) {
      /* do nothing */
    }
    else if (tok == TokCloseArray) {
      return 0;
    }
    else {
      print_msg(p, "Error: unexpected token \"%s\", expected comma or "
        "end of array\n", str);
      goto fail;
    }
  }

  fail: return -1;
}

/* Parses the right-hand side of an assignment (after the equal sign).
 * Checks for any preceeding optional cast, and then parses the value of the
 * assignment.  Terminates when the trailing semicolon is found.
 */
static int parse_right_side(Parser * p, BotParamElement * el)
{
  BotParamToken tok;
  char str[256];

  if (get_token(p, &tok, str, sizeof(str)) != 0)
    goto fail;

  /* Allow an optional cast preceeding the right-hand side */
  if (tok == TokCast) {
    /* Cast is currently ignored */
    if (get_token(p, &tok, str, sizeof(str)) != 0)
      goto fail;
  }

  if (tok == TokIdentifier || tok == TokString) {
    add_value(p, el, str);
  }
  else if (tok == TokOpenArray) {
    if (parse_array(p, el) < 0)
      goto fail;
  }
  else {
    print_msg(p, "Error: unexpected token \"%s\", expected right-hand "
      "side\n", str);
    goto fail;
  }

  if (get_token(p, &tok, str, sizeof(str)) != 0)
    goto fail;

  if (tok != TokEndStatement) {
    print_msg(p, "Error: unexpected token \"%s\", expected semicolon\n", str);
    goto fail;
  }

  return 0;

  fail: return -1;
}

/* Parses the interior a container (the portion after the "{").  Any
 * assignment statements or enclosed containers are recursively parsed.
 * Terminates when end_token is found, which should be TokEOF for the
 * top-level container and TokCloseStruct for everything else. */
static int parse_container(Parser * p, BotParamElement * cont, BotParamToken end_token)
{
  BotParamToken tok;
  char str[256];
  BotParamElement * child = NULL;

  while (get_token(p, &tok, str, sizeof(str)) == 0) {
    //printf ("t %d: %s\n", tok, str);
    if (!child && tok == TokIdentifier) {
      child = new_element(str);
    }
    else if (child && tok == TokAssign) {
      child->type = BotParamArray;
      if (parse_right_side(p, child) < 0)
        goto fail;
      add_child(p, cont, child);
      child = NULL;
    }
    else if (child && tok == TokOpenStruct) {
      child->type = BotParamContainer;
      if (parse_container(p, child, TokCloseStruct) < 0)
        goto fail;
      add_child(p, cont, child);
      child = NULL;
    }
    else if (!child && tok == end_token)
      return 0;
    else {
      print_msg(p, "Error: unexpected token \"%s\"\n", str);
      goto fail;
    }
  }

  fail: if (child) {
    free_element(child);
  }
  return -1;
}

static int write_array(BotParamElement * el, int indent, FILE * f)
{
  fprintf(f, "%*s%s = [", indent, "", el->name);

  int i;
  for (i = 0; i < el->num_values; i++) {
    fprintf(f, "\"%s\", ", el->values[i]);
  }
  fprintf(f, "];\n");
  return 0;
}

static int write_container(BotParamElement * el, int indent, FILE * f)
{
  BotParamElement * child;

  fprintf(f, "%*s%s {\n", indent, "", el->name);

  for (child = el->children; child; child = child->next) {
    if (child->type == BotParamContainer)
      write_container(child, indent + 4, f);
    else if (child->type == BotParamArray)
      write_array(child, indent + 4, f);
    else {
      fprintf(stderr, "Error: unknown child (%d)\n", child->type);
      return -1;
    }
  }

  fprintf(f, "%*s}\n", indent, "");
  return 0;
}

/* Prints the contents of a configuration file's parse tree to the file handle
 * f. */
int bot_param_write(BotParam * param, FILE * f)
{
  g_mutex_lock(param->lock);
  BotParamElement * child, *root;

  root = param->root;

  for (child = root->children; child; child = child->next) {
    if (child->type == BotParamContainer)
      write_container(child, 0, f);
    else if (child->type == BotParamArray)
      write_array(child, 0, f);
    else {
      fprintf(stderr, "Error: unknown child (%d)\n", child->type);
      g_mutex_unlock(param->lock);
      return -1;
    }
  }
  g_mutex_unlock(param->lock);
  return 0;
}

/*
 * only used internally
 */
static BotParam * _bot_param_new(void)
{
  BotParamElement * root;
  root = new_element(NULL);
  root->type = BotParamContainer;

  BotParam * param;
  param = calloc(1, sizeof(BotParam));
  param->root = root;
  param->lock = g_mutex_new();
  param->server_id = 0;
  param->sequence_number = 0;

  return param;
}

void bot_param_destroy(BotParam * param)
{
  free_element(param->root);
  g_mutex_free(param->lock);
  free(param);
}

static void _on_param_update(const lcm_recv_buf_t *rbuf, const char * channel, const bot_param_update_t * msg,
    void * user)
{
  BotParam * param = (BotParam *) user;
  if (param->server_id <= 0) {
    param->server_id = msg->server_id;
    param->sequence_number = msg->sequence_number - 1;
  }
  if (msg->server_id == param->server_id) {
    if (msg->sequence_number <= param->sequence_number)
     	return;
//    else 
//	fprintf(stderr, "received NEW params from server:\n");
  }
  else {
    fprintf(stderr, "WARNING: Got params from a different server! Ignoring them\n");
    return;
  }

  BotParam * tmp = bot_param_new_from_string(msg->params, strlen(msg->params));
  if (tmp == NULL) {
    fprintf(stderr, "WARNING: Could not parse params from the server!\n");
    return;
  }
  //swap the root;
  g_mutex_lock(param->lock);
  param->sequence_number = msg->sequence_number;
  BotParamElement * root = tmp->root;
  tmp->root = param->root;
  param->root = root;
  bot_param_destroy(tmp);
  g_mutex_unlock(param->lock);
}

BotParam * bot_param_new_from_server(lcm_t * lcm, int keep_updated)
{
  BotParam * param = _bot_param_new();

  //create a temporary lcm object to be sure nothing else is subscribed...
  lcm_t * lcm_tmp = lcm_create(NULL); //TOOD: figure out how to handle other LCM providers...

  bot_param_update_t_subscribe(lcm_tmp, PARAM_UPDATE_CHANNEL, _on_param_update, (void *) param);
  for (int i = 0; i < 5; i++) {
    bot_param_request_t req;
    req.utime = _timestamp_now();
    bot_param_request_t_publish(lcm_tmp, PARAM_REQUEST_CHANNEL, &req);
    lcm_sleep(lcm_tmp, 1);
    if (param->root->children != NULL)
      break;
  }
  lcm_destroy(lcm_tmp); //got the first parameter set, don't need this anymore

  if (param->root->children == NULL) {
    fprintf(stderr, "WARNING: Could not get parameters from the param-server! did you forget to start one?\n");
    return NULL;
  }
  if (keep_updated) {
    bot_param_update_t_subscribe(lcm, PARAM_UPDATE_CHANNEL, _on_param_update, (void *) param);
    //the handler will update the param structure every time a message arrives
  }
  return param;

}

BotParam * bot_param_new_from_file(const char * filename)
{
  ParserFile pf;
  FILE * f = fopen(filename, "r");
  if (f == NULL) {
    err("could not open param file: %s\n",filename);
    return NULL;
  }

  memset(&pf, 0, sizeof(ParserFile));
  pf.p.get_ch = get_ch_file;
  pf.p.type = ParserTypeFile;
  pf.file = f;
  pf.filename = filename;

  BotParam * param = _bot_param_new();
  if (parse_container(&pf.p, param->root, TokEOF) < 0) {
    bot_param_destroy(param);
    return NULL;
  }
  else {
    return param;
  }
}

BotParam * bot_param_new_from_string(const char * string, int length)
{
  ParserString ps;

  memset(&ps, 0, sizeof(ParserString));
  ps.p.get_ch = get_ch_string;
  ps.p.type = ParserTypeString;
  ps.string = string;
  ps.length = length;

  BotParam * param = _bot_param_new();
  if (parse_container(&ps.p, param->root, TokEOF) < 0) {
    bot_param_destroy(param);
    return NULL;
  }
  else {
    return param;
  }
}

static BotParamElement *
find_key(BotParamElement * el, const char * key, int inherit)
{
  size_t len = strcspn(key, ".");
  char str[len + 1];
  memcpy(str, key, len);
  str[len] = '\0';

  const char * remainder = NULL;
  if (key[len] == '.')
    remainder = key + len + 1;

  BotParamElement * child;
  for (child = el->children; child; child = child->next) {
    if (!strcmp(str, child->name)) {
      if (remainder)
        return find_key(child, remainder, inherit);
      else
        return child;
    }
  }
  if (inherit && !remainder && el->parent)
    return find_key(el->parent, str, inherit);
  else
    return NULL;
}

static int cast_to_int(const char * key, const char * val, int * out)
{
  char * end;
  *out = strtol(val, &end, 0);
  if (end == val || *end != '\0') {
    fprintf(stderr, "Error: key \"%s\" (\"%s\") did not cast "
      "properly to int\n", key, val);
    return -1;
  }
  return 0;
}

static int cast_to_boolean(const char * key, const char * val, int * out)
{
  if (!strcasecmp(val, "y") || !strcasecmp(val, "yes") || !strcasecmp(val, "true") || !strcmp(val, "1"))
    *out = 1;
  else if (!strcasecmp(val, "n") || !strcasecmp(val, "no") || !strcasecmp(val, "false") || !strcmp(val, "0"))
    *out = 0;
  else {
    fprintf(stderr, "Error: key \"%s\" (\"%s\") did not cast "
      "properly to boolean\n", key, val);
    return -1;
  }
  return 0;
}

static double cast_to_double(const char * key, const char * val, double * out)
{
  char * end;
  *out = strtod(val, &end);
  if (end == val || *end != '\0') {
    fprintf(stderr, "Error: key \"%s\" (\"%s\") did not cast "
      "properly to double\n", key, val);
    return -1;
  }
  return 0;
}

#define PRINT_KEY_NOT_FOUND(key) \
    err("WARNING: BotParam: could not find key %s!\n", (key));

int bot_param_has_key(BotParam *param, const char *key)
{
  g_mutex_lock(param->lock);
  int ret = (find_key(param->root, key, 1) != NULL);
  g_mutex_unlock(param->lock);
  return ret;
}

int bot_param_get_num_subkeys(BotParam * param, const char * containerKey)
{
  g_mutex_lock(param->lock);

  BotParamElement* el = param->root;
  if ((NULL != containerKey) && (0 < strlen(containerKey)))
    el = find_key(param->root, containerKey, 1);
  if (NULL == el) {
    g_mutex_unlock(param->lock);
    return -1;
  }

  int count = 0;
  BotParamElement* child;
  for (child = el->children; child; child = child->next)
    ++count;

  g_mutex_unlock(param->lock);

  return count;
}

char **
bot_param_get_subkeys(BotParam * param, const char * containerKey)
{
  g_mutex_lock(param->lock);

  BotParamElement* el = param->root;
  if ((NULL != containerKey) && (0 < strlen(containerKey)))
    el = find_key(param->root, containerKey, 1);
  if (NULL == el) {
    g_mutex_unlock(param->lock);
    return NULL;
  }

  int count = 0;
  BotParamElement* child;
  for (child = el->children; child; child = child->next, ++count)
    ;

  char **result = calloc(count + 1, sizeof(char*));

  int i = 0;
  for (child = el->children; child; child = child->next) {
    result[i] = strdup(child->name);
    i++;
  }
  g_mutex_unlock(param->lock);
  return result;
}

int bot_param_get_int(BotParam * param, const char * key, int * val)
{
  g_mutex_lock(param->lock);

  BotParamElement * el = find_key(param->root, key, 1);
  if (!el || el->type != BotParamArray || el->num_values < 1) {
    g_mutex_unlock(param->lock);
    return -1;
  }
  int ret = cast_to_int(key, el->values[0], val);

  g_mutex_unlock(param->lock);
  return ret;
}

int bot_param_get_boolean(BotParam * param, const char * key, int * val)
{
  g_mutex_lock(param->lock);
  BotParamElement * el = find_key(param->root, key, 1);
  if (!el || el->type != BotParamArray || el->num_values < 1) {
    g_mutex_unlock(param->lock);
    return -1;
  }

  int ret = cast_to_boolean(key, el->values[0], val);
  g_mutex_unlock(param->lock);
  return ret;
}

int bot_param_get_double(BotParam * param, const char * key, double * val)
{
  g_mutex_lock(param->lock);

  BotParamElement * el = find_key(param->root, key, 1);
  if (!el || el->type != BotParamArray || el->num_values < 1) {
    return -1;
  }
  double ret = cast_to_double(key, el->values[0], val);

  g_mutex_unlock(param->lock);
  return ret;
}

int bot_param_get_str(BotParam * param, const char * key, char ** val)
{
  g_mutex_lock(param->lock);

  BotParamElement * el = find_key(param->root, key, 1);
  if (!el || el->type != BotParamArray || el->num_values < 1) {
    g_mutex_unlock(param->lock);
    return -1;
  }
  *val = strdup(el->values[0]);
  g_mutex_unlock(param->lock);
  return 0;
}

int bot_param_get_int_or_fail(BotParam * param, const char * key)
{
  int val;
  if (bot_param_get_int(param, key, &val) == 0)
    return val;
  else {
    fprintf(stderr, "Missing config key: %s\n", key);
    abort();
  }
}

int bot_param_get_boolean_or_fail(BotParam * param, const char * key)
{
  int val;
  if (bot_param_get_boolean(param, key, &val) == 0)
    return val;
  else {
    fprintf(stderr, "Missing config key: %s\n", key);
    abort();
  }
}

double bot_param_get_double_or_fail(BotParam *param, const char *key)
{
  double v;
  int res = bot_param_get_double(param, key, &v);
  if (res) {
    fprintf(stderr, "Missing config key: %s\n", key);
    abort();
  }

  return v;
}

char *bot_param_get_str_or_fail(BotParam *param, const char *key)
{
  char * str;
  if (bot_param_get_str(param, key, &str) == 0)
    return str;
  else {
    fprintf(stderr, "Missing config key: %s\n", key);
    abort();
  }
}

int bot_param_get_int_array(BotParam * param, const char * key, int * vals, int len)
{
  g_mutex_lock(param->lock);

  BotParamElement * el = find_key(param->root, key, 1);
  if (!el || el->type != BotParamArray) {
    g_mutex_unlock(param->lock);
    return -1;
  }
  int i;
  for (i = 0; i < el->num_values; i++) {
    if (len != -1 && i == len)
      break;
    if (cast_to_int(key, el->values[i], vals + i) < 0) {
      err("WARNING: BotParam: cast error parsing int array %s\n", key);
      g_mutex_unlock(param->lock);
      return -1;
    }
  }
  if (i < len) {
    err("WARNING: BotParam: only read %d of %d values for integer array\n"
        "         %s\n", i, len, key);
  }

  g_mutex_unlock(param->lock);

  return i;
}


void bot_param_get_int_array_or_fail(BotParam * param, const char * key, int * vals, int len)
{
    int res = bot_param_get_int_array(param, key, vals, len);
    if (res != len) {
        fprintf (stderr, "ERROR: BotParam: only read %d of %d integer values for key: %s\n", res, len, key);
        abort ();
    }

    return;
}


int bot_param_get_boolean_array(BotParam * param, const char * key, int * vals, int len)
{
  g_mutex_lock(param->lock);

  BotParamElement * el = find_key(param->root, key, 1);
  if (!el || el->type != BotParamArray) {
    g_mutex_unlock(param->lock);
    return -1;
  }
  int i;
  for (i = 0; i < el->num_values; i++) {
    if (len != -1 && i == len)
      break;
    if (cast_to_boolean(key, el->values[i], vals + i) < 0) {
      err("WARNING: BotParam: cast error parsing boolean array %s\n", key);
      g_mutex_unlock(param->lock);
      return -1;
    }
  }
  if (i < len) {
    err("WARNING: BotParam: only read %d of %d values for boolean array\n"
        "         %s\n", i, len, key);
  }

  g_mutex_unlock(param->lock);

  return i;
}

void bot_param_get_boolean_array_or_fail(BotParam * param, const char * key, int * vals, int len)
{
    int res = bot_param_get_boolean_array(param, key, vals, len);
    if (res != len) {
        fprintf (stderr, "ERROR: BotParam: only read %d of %d boolean values for key: %s\n", res, len, key);
        abort ();
    }

    return;
}


int bot_param_get_double_array(BotParam * param, const char * key, double * vals, int len)
{
  g_mutex_lock(param->lock);

  BotParamElement * el = find_key(param->root, key, 1);
  if (!el || el->type != BotParamArray) {
    g_mutex_unlock(param->lock);
    return -1;
  }
  int i;
  for (i = 0; i < el->num_values; i++) {
    if (len != -1 && i == len)
      break;
    if (cast_to_double(key, el->values[i], vals + i) < 0) {
      err("WARNING: BotParam: cast error parsing double array %s\n", key);
      g_mutex_unlock(param->lock);
      return -1;
    }
  }
  if (i < len) {
    err("WARNING: BotParam: only read %d of %d values for double array\n"
        "         %s\n", i, len, key);
  }

  g_mutex_unlock(param->lock);
  return i;
}

void bot_param_get_double_array_or_fail(BotParam * param, const char * key, double * vals, int len)
{
    int res = bot_param_get_double_array(param, key, vals, len);
    if (res != len) {
        fprintf (stderr, "ERROR: BotParam: only read %d of %d double values for key: %s\n\n", res, len, key);
        abort ();
    }

    return;
}


int bot_param_get_array_len(BotParam *param, const char * key)
{
  g_mutex_lock(param->lock);
  BotParamElement * el = find_key(param->root, key, 1);
  if (!el || el->type != BotParamArray) {
    g_mutex_unlock(param->lock);
    return -1;
  }
  int ret = el->num_values;

  g_mutex_unlock(param->lock);
  return ret;
}

char **
bot_param_get_str_array_alloc(BotParam * param, const char * key)
{
  g_mutex_lock(param->lock);

  BotParamElement * el = find_key(param->root, key, 1);
  if (!el || el->type != BotParamArray) {
    g_mutex_unlock(param->lock);
    return NULL;
  }

  // + 1 so that the list is null terminated.
  char **data = calloc(el->num_values + 1, sizeof(char*));

  int i;
  for (i = 0; i < el->num_values; i++) {
    data[i] = strdup(el->values[i]);
  }

  g_mutex_unlock(param->lock);

  return data;
}

void bot_param_str_array_free(char **data)
{
  int idx = 0;
  while (data[idx] != NULL)
    free(data[idx++]);

  free(data);
}

static BotParamElement *
create_key(BotParamElement * el, const char * key)
{
  size_t len = strcspn(key, ".");
  char str[len + 1];
  memcpy(str, key, len);
  str[len] = '\0';

  const char * remainder = NULL;
  if (key[len] == '.')
    remainder = key + len + 1;

  BotParamElement * child;
  for (child = el->children; child; child = child->next) {
    if (!strcmp(str, child->name)) {
      if (remainder)
        return create_key(child, remainder);
      else
        return child;
    }
  }

  child = new_element(str);
  add_child(NULL, el, child);
  if (remainder) {
    child->type = BotParamContainer;
    return create_key(child, remainder);
  }
  else {
    child->type = BotParamArray;
    return child;
  }
}

/*
 * Functions for setting key/value pairs
 */

static int set_value(BotParam * param, const char * key, const char * val)
{
  g_mutex_lock(param->lock);

  BotParamElement * el = find_key(param->root, key, 0);
  if (el == NULL)
    el = create_key(param->root, key);
  else if (el->type != BotParamArray) {
    g_mutex_unlock(param->lock);
    return -1;
  }

  if (el->num_values < 1)
    add_value(NULL, el, val);
  else {
    free(el->values[0]);
    el->values[0] = strdup(val);
  }

  g_mutex_unlock(param->lock);
  return 1;
}

int bot_param_set_int(BotParam * param, const char * key, int val)
{
  char str[16];
  sprintf(str, "%d", val);
  return set_value(param, key, str);
}

int bot_param_set_boolean(BotParam * param, const char * key, int val)
{
  return set_value(param, key, (val == 0 ? "false" : "true"));
}

int bot_param_set_double(BotParam * param, const char * key, double val)
{
  char str[32];
  sprintf(str, "%f", val);
  return set_value(param, key, str);
}

int bot_param_set_str(BotParam * param, const char * key, char * val)
{
  return set_value(param, key, val);
}

/*
 * Functions for setting array of values
 */

int bot_param_set_int_array(BotParam * param, const char * key, int * vals, int len)
{
  char* str;
  char single_val[16];
  int string_len = 1;
  int single_len;
  int i;

  str = malloc(1);
  str[0] = '\0';
  for (i = 0; i < len; ++i) {
    if (i < len - 1)
      sprintf(single_val, "%d,", vals[i]);
    else
      sprintf(single_val, "%d", vals[i]);
    single_len = strlen(single_val);
    str = realloc(str, string_len + single_len);
    strcat(str, single_val);
    string_len += single_len;
  }

  int ret_val = set_value(param, key, str);
  free(str);
  return ret_val;
}

int bot_param_set_boolean_array(BotParam * param, const char * key, int * vals, int len)
{
  char* str;
  char single_val[16];
  int string_len = 1;
  int single_len;
  int i;
  char val_str[8];

  str = malloc(1);
  str[0] = '\0';
  for (i = 0; i < len; ++i) {
    strcpy(val_str, (vals[i] == 0 ? "false" : "true"));
    if (i < len - 1)
      sprintf(single_val, "%s,", val_str);
    else
      sprintf(single_val, "%s", val_str);
    single_len = strlen(single_val);
    str = realloc(str, string_len + single_len);
    strcat(str, single_val);
    string_len += single_len;
  }

  int ret_val = set_value(param, key, str);
  free(str);
  return ret_val;
}

int bot_param_set_double_array(BotParam * param, const char * key, double * vals, int len)
{
  char* str;
  char single_val[32];
  int string_len = 1;
  int single_len;
  int i;

  str = malloc(1);
  str[0] = '\0';
  for (i = 0; i < len; ++i) {
    if (i < len - 1)
      sprintf(single_val, "%f,", vals[i]);
    else
      sprintf(single_val, "%f", vals[i]);
    single_len = strlen(single_val);
    str = realloc(str, string_len + single_len);
    strcat(str, single_val);
    string_len += single_len;
  }

  int ret_val = set_value(param, key, str);
  free(str);
  return ret_val;
}

int bot_param_set_str_array(BotParam * param, const char * key, char ** vals, int len)
{
  char* str;
  char single_val[256];
  int string_len = 1;
  int single_len;
  int i;

  str = malloc(1);
  str[0] = '\0';
  for (i = 0; i < len; ++i) {
    if (i < len - 1)
      sprintf(single_val, "%s,", vals[i]);
    else
      sprintf(single_val, "%s", vals[i]);
    single_len = strlen(single_val);
    str = realloc(str, string_len + single_len);
    strcat(str, single_val);
    string_len += single_len;
  }

  int ret_val = set_value(param, key, str);
  free(str);
  return ret_val;
}

int64_t bot_param_client_get_server_id(BotParam * param)
{
  g_mutex_lock(param->lock);
  int64_t ret = param->server_id;
  g_mutex_unlock(param->lock);
  return ret;
}

int bot_param_client_get_seqno(BotParam * param)
{
  g_mutex_lock(param->lock);
  int ret = param->sequence_number;
  g_mutex_unlock(param->lock);
  return ret;
}

static lcm_t *global_lcm = NULL;
static BotParam *global_param = NULL;
static int64_t global_param_refcount = 0;
static GStaticMutex bot_param_global_mutex = G_STATIC_MUTEX_INIT;

BotParam*
bot_param_get_global(int keep_updated)
{
    g_static_mutex_lock (&bot_param_global_mutex);

    if (keep_updated)
        global_lcm = bot_lcm_get_global ();

    if (global_param_refcount == 0) {
        assert (!global_param);

        if (keep_updated)
            global_param = bot_param_new_from_server (global_lcm, keep_updated);
        else
            global_param = bot_param_new_from_server (NULL, keep_updated);

        if (!global_param)
            goto fail;
    }

    assert (global_param);

    if (global_param_refcount < MAX_REFERENCES)
        global_param_refcount++;

    BotParam *result = global_param;
    g_static_mutex_unlock (&bot_param_global_mutex);
    return result;

 fail:
    g_static_mutex_unlock (&bot_param_global_mutex);
    fprintf (stderr, "ERROR: Could not get global BotParam!\n");
    return NULL;
}

void
bot_param_release_global(BotParam *param)
{
    g_static_mutex_lock (&bot_param_global_mutex);

    if (global_param_refcount == 0) {
        fprintf (stderr, "ERROR: singleton param refcount already zero!\n");
        g_static_mutex_unlock (&bot_param_global_mutex);
        return;
    }

    if (param != global_param)
        fprintf (stderr, "ERROR: %p is not the singleton BotParam (%p)\n", param, global_param);

    global_param_refcount--;

    if (global_param_refcount == 0) {
        bot_param_destroy (global_param);
        global_param = NULL;
        
        if (global_lcm) {
            lcm_destroy (global_lcm);
            global_lcm = NULL;
        }
    }
    g_static_mutex_unlock (&bot_param_global_mutex);
}