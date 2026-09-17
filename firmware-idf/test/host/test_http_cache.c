/* #RADEX-294: хост-тест разбора Accept-Encoding / If-None-Match для страницы "/".
   Образцы заголовков — в том виде, в каком их шлют браузеры. */
#include <stdio.h>
#include <stdbool.h>
#include "http_cache.h"

static int fail_tests = 0, total_tests = 0;
#define TEST(name, cond) do { total_tests++; bool ok_ = (cond); if (!ok_) fail_tests++; \
    printf("%s %s\n", ok_ ? "GREEN" : "RED  ", name); } while (0)

#define ETAG "\"abc123\""

int main(void)
{
    TEST("test_ae_chrome", accept_gzip("gzip, deflate, br, zstd") == true);
    TEST("test_ae_firefox", accept_gzip("gzip, deflate, br") == true);
    TEST("test_ae_no_header", accept_gzip(NULL) == false);
    TEST("test_ae_without_gzip", accept_gzip("deflate, br") == false);
    TEST("test_ae_gzip_q0", accept_gzip("br, gzip;q=0") == false);
    TEST("test_ae_gzip_q_half", accept_gzip("br;q=1.0, gzip;q=0.5") == true);
    TEST("test_ae_identity", accept_gzip("identity") == false);
    TEST("test_ae_token_not_prefix", accept_gzip("gzipx") == false);
    TEST("test_inm_strong", etag_match("\"abc123\"", ETAG) == true);
    TEST("test_inm_weak", etag_match("W/\"abc123\"", ETAG) == true);
    TEST("test_inm_list", etag_match("\"x\", \"abc123\"", ETAG) == true);
    TEST("test_inm_star", etag_match("*", ETAG) == true);
    TEST("test_inm_other", etag_match("\"abc124\"", ETAG) == false);
    TEST("test_inm_prefix", etag_match("\"abc12\"", ETAG) == false);
    TEST("test_inm_unquoted_rejected", etag_match("abc123", ETAG) == false);
    TEST("test_inm_no_header", etag_match(NULL, ETAG) == false);
    TEST("test_inm_page_etag_form", etag_match("\"9f86d081884c7d65\"", "\"9f86d081884c7d65\"") == true);
    printf("итого (http_cache): красных тестов %d из %d\n", fail_tests, total_tests);
    return fail_tests ? 1 : 0;
}
