#ifndef UTIL_H
#define UTIL_H

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define UNUSED(x) (void)x

typedef struct ArrayBuffer ArrayBuffer;
typedef struct StrView StrView;
typedef struct Span Span;
typedef struct ObjectPool ObjectPool;
typedef struct RelPtr RelPtr;
typedef struct Allocator Allocator;
typedef struct FileBuffer FileBuffer;
typedef struct WorkGroup WorkGroup;
typedef uint64_t PCG32State;

typedef uint_fast32_t ObjectID;

struct WorkGroup {
	void *queue;
	void *queue_begin, *queue_end;
	size_t size, enqueued;
	size_t elem_size;
	bool terminated;

	size_t worker_count;
	void (*worker_func)(WorkGroup *);
	pthread_mutex_t mtx;
	pthread_cond_t cond;
	pthread_t workers[];
};

struct Allocator {
	void *userptr;
	void *(*allocate)(size_t bytes, void *user_ptr);
	void  (*deallocate)(void *ptr, void *user_ptr);
};

struct ArrayBuffer {
	bool initialized;
	size_t size;
	size_t reserved;
	void *data;
	Allocator allocator;
};

struct StrView {
	const unsigned char *begin;
	const unsigned char *end;
};

struct ObjectPool {
	ArrayBuffer pages;
	ArrayBuffer free_stack;
	ArrayBuffer dirty_buffer;
	void *object_list;

	size_t node_size;
	size_t obj_size;
	size_t alignment;

	void (*clean_cbk)(ObjectPool *, void*);
};

struct FileBuffer {
	void *file_handle;
	ArrayBuffer data_buffer;
};

struct Span {
	void *begin;
	void *end;
};

struct RelPtr {
	void **base_pointer;
	uintptr_t offset;
};

#if defined(__GNUC__)
	#define ALIGNMENT_OF(X) __alignof__(X)
#elif defined(_MSC_VER)
	#define ALIGNMENT_OF(X) alignof(X)
#else
	#error "No alignment defined!"
#endif

#define LENGTH(ARR) (sizeof(ARR) / sizeof(ARR)[0])
#define ASSERT(CHECK) if(!(CHECK)) { die("%s:%d: '%s' failed\n", __FILE__, __LINE__, #CHECK); }

#define SPAN_FOR(SPAN, NAME, ...) for(__VA_ARGS__* NAME = SPAN.begin; NAME < (__VA_ARGS__*)SPAN.end; NAME++)

#define CONTAINER_OF(PTR, STRUCT, MEMBER) \
	((STRUCT*)((uintptr_t)(PTR) - offsetof(STRUCT, MEMBER)))

#define emalloc(size) _emalloc(size, __FILE__, __LINE__)
#define efree(ptr) _efree(ptr, __FILE__, __LINE__)
#define erealloc(ptr, size) _erealloc(ptr, size, __FILE__, __LINE__)

void   arrbuf_init(ArrayBuffer *buffer);
void   arrbuf_init_allocator(ArrayBuffer *buffer, Allocator allocator);

void   arrbuf_reserve(ArrayBuffer *buffer,   size_t element_size);
void   arrbuf_insert(ArrayBuffer *buffer,    size_t element_size, const void *data);
void   arrbuf_insert_at(ArrayBuffer *buffer, size_t element_size, const void *data, size_t pos);
void   arrbuf_remove(ArrayBuffer *buffer,    size_t element_size, size_t pos);
size_t arrbuf_length(ArrayBuffer *buffer,    size_t element_size);
void   arrbuf_clear(ArrayBuffer *buffer);
Span   arrbuf_span(ArrayBuffer *buffer);

void   *arrbuf_peektop(ArrayBuffer *buffer, size_t element_size);
void    arrbuf_poptop(ArrayBuffer *buffer, size_t element_size);
void   arrbuf_free(ArrayBuffer *buffer);

void arrbuf_printf(ArrayBuffer *buffer, const char *fmt, ...);

/* 
 * use only for IN-PLACE STRUCT FILLING, do not save the pointer, 
 * IT WILL become a dangling pointer after a realloc if you are not using 
 * the stack, example at arrbuf_insert
 */
void *arrbuf_newptr(ArrayBuffer *buffer, size_t element_size);
void *arrbuf_newptr_at(ArrayBuffer *buffer, size_t element_size, size_t pos);

int   fbuf_open(FileBuffer *buffer, const char *path, const char *mode, Allocator alloc);
int   fbuf_read(FileBuffer *buffer, size_t size);
int   fbuf_write(FileBuffer *buffer, size_t size, void *ptr);
int   fbuf_flush(FileBuffer *buffer);
void  fbuf_close(FileBuffer *buffer);
char *fbuf_data(FileBuffer *buffer);
int   fbuf_data_size(FileBuffer *buffer);
int  fbuf_read_line(FileBuffer *buffer, int delim);
StrView fbuf_data_view(FileBuffer *buffer);

StrView to_strview(const char *str);
StrView to_strview_buffer(const void *buffer, size_t size);
StrView strview_token(StrView *str, const char *delim);
int     strview_cmp(StrView str, const char *str2);
int     strview_cmpstr(StrView str, StrView str2);
char   *strview_str(StrView view);
void    strview_str_mem(StrView view, char *data, size_t size);

int strview_int(StrView str, int *result);
int strview_float(StrView str, float *result);

void die(const char *fmt, ...);
char *read_file(const char *path, size_t *size);

void *_emalloc(size_t size, const char *file, int line);
void  _efree(void *ptr, const char *file, int line);
void *_erealloc(void *ptr, size_t size, const char *file, int line);

/* 
 * alignment needs to be a multiple of sizeof(void*) the same as posix_memalign() 
 * use DEFAULT_ALIGNMENT if you don't care about this (which is likely) 
 */
void objpool_init(ObjectPool *pool, size_t object_size, size_t object_alignment);
void objpool_clean(ObjectPool *pool);
void objpool_reset(ObjectPool *pool);
void objpool_terminate(ObjectPool *pool);

void *objpool_begin(ObjectPool *pool);
void *objpool_next(void *data);

void *objpool_new(ObjectPool *pool);
void  objpool_free(void *object_ptr);
bool  objpool_is_dead(void *object_ptr);

Allocator allocator_default(void);

void *alloct_allocate(Allocator *, size_t size);
void  alloct_deallocate(Allocator *, void *ptr);

int  utf8_decode(StrView span);
void utf8_advance(StrView *span);
int  utf8_multibyte_next(StrView view, int from);
int  utf8_multibyte_prev(StrView view, int from);

WorkGroup *wg_init(void (*worker_func)(WorkGroup *wg), size_t work_data_size, size_t max_work_count, size_t worker_count);
void       wg_terminate(WorkGroup *wg);
bool       wg_send(WorkGroup *wg, void *work_data);
bool       wg_recv(WorkGroup *wg, void *data_in);
bool       wg_recv_nonblock(WorkGroup *wg, void *data);

uint32_t rotr32(uint32_t x, int r);
uint32_t rotl32(uint32_t x, int r);
uint64_t hash_string(const char *str);

void init_pcg32(PCG32State *state);
uint32_t rand_pcg32(PCG32State *state);

static inline void *to_ptr(RelPtr ptr) {
	return ((unsigned char*)*ptr.base_pointer) + ptr.offset;
}

static inline int mini(int a, int b) {
	return a < b ? a : b;
}

static inline int maxi(int a, int b) {
	return a > b ? a : b;
}

static inline int clampi(int x, int minv, int maxv) {
	return mini(maxi(x, minv), maxv);
}

static inline uint32_t hash_int(uint32_t i) {
    i *= 3551332717;
	i ^= ((i >> 15) | (i << 17));
	return i;
}

static inline uint32_t hash_int3(uint32_t x, uint32_t y, uint32_t z) {
	uint32_t h       = 4075758091;
	const uint32_t m = 3757705307;

	h = (hash_int(x) ^ h) * m;
	h = (hash_int(y) ^ h) * m;
	h = (hash_int(z) ^ h) * m;
	return h;
}

#define DEFAULT_ALIGNMENT (sizeof(void*))

#endif
