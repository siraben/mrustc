#include <stdint.h>
#include <stdlib.h>

uint32_t probe_alloc_string(uint32_t count);

/* alloc's global allocator hooks (no std): map to malloc/free. */
void *__rust_alloc(size_t size, size_t align) { (void)align; return malloc(size); }
void __rust_dealloc(void *ptr, size_t size, size_t align) { (void)size; (void)align; free(ptr); }
void *__rust_realloc(void *ptr, size_t old, size_t align, size_t new_size) { (void)old; (void)align; return realloc(ptr, new_size); }
void *__rust_alloc_zeroed(size_t size, size_t align) { (void)align; return calloc(1, size); }
/* core's panic handler and alloc's shim marker, declared in extern blocks. */
long long panic_impl(long long info) { (void)info; abort(); }
void __rust_no_alloc_shim_is_unstable_v2(void) {}
void __rust_alloc_error_handler(size_t size, size_t align) { (void)size; (void)align; abort(); }

int main(void)
{
    /* count=3: parts "0-0","1-10","2-20" -> joined "0-0;1-10;2-20;" (14) + "3!" (2) = 16;
       first bytes '0'+'1'+'2' = 48+49+50 = 147; boxed 21 */
    if (probe_alloc_string(3) != 16000 + 147 + 21) return 1;
    /* count=0: "0!" -> 2 */
    if (probe_alloc_string(0) != 2000) return 2;
    return 0;
}
