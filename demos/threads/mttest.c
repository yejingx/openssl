/* Copyright (C) 1995-1998 Eric Young (eay@cryptsoft.com)
 * All rights reserved.
 *
 * This package is an SSL implementation written
 * by Eric Young (eay@cryptsoft.com).
 * The implementation was written so as to conform with Netscapes SSL.
 *
 * This library is free for commercial and non-commercial use as long as
 * the following conditions are aheared to.  The following conditions
 * apply to all code found in this distribution, be it the RC4, RSA,
 * lhash, DES, etc., code; not just the SSL code.  The SSL documentation
 * included with this distribution is covered by the same copyright terms
 * except that the holder is Tim Hudson (tjh@cryptsoft.com).
 *
 * Copyright remains Eric Young's, and as such any Copyright notices in
 * the code are not to be removed.
 * If this package is used in a product, Eric Young should be given attribution
 * as the author of the parts of the library used.
 * This can be in the form of a textual message at program startup or
 * in documentation (online or textual) provided with the package.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. All advertising materials mentioning features or use of this software
 *    must display the following acknowledgement:
 *    "This product includes cryptographic software written by
 *     Eric Young (eay@cryptsoft.com)"
 *    The word 'cryptographic' can be left out if the rouines from the library
 *    being used are not cryptographic related :-).
 * 4. If you include any Windows specific code (or a derivative thereof) from
 *    the apps directory (application code) you must include an acknowledgement:
 *    "This product includes software written by Tim Hudson (tjh@cryptsoft.com)"
 *
 * THIS SOFTWARE IS PROVIDED BY ERIC YOUNG ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 * The licence and distribution terms for any publically available version or
 * derivative of this code cannot be changed.  i.e. this code cannot simply be
 * copied and put under another distribution licence
 * [including the GNU Public Licence.]
 */

#include <stdlib.h>
#include <string.h>
#include <errno.h>
#ifdef LINUX
# include <typedefs.h>
#endif
#ifdef OPENSSL_SYS_WIN32
# include <windows.h>
#endif
#ifdef PTHREADS
# include <pthread.h>
#endif
#include <openssl/lhash.h>
#include <openssl/crypto.h>
#include <openssl/buffer.h>
#include <openssl/x509.h>
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/rand.h>

#define TEST_SERVER_CERT "../../apps/server.pem"
#define TEST_CLIENT_CERT "../../apps/client.pem"

#define MAX_THREAD_NUMBER       100

int verify_callback(int ok, X509_STORE_CTX *xs);
void thread_setup(void);
void thread_cleanup(void);
void do_threads(SSL_CTX *s_ctx, SSL_CTX *c_ctx);

void win32_locking_callback(int mode, int type, const char *file, int line);
void pthreads_locking_callback(int mode, int type, const char *file, int line);

void irix_thread_id(CRYPTO_THREADID *tid);
void pthreads_thread_id(CRYPTO_THREADID *tid);

BIO *bio_err = NULL;
BIO *bio_stdout = NULL;

static char *cipher = NULL;
int verbose = 0;
#ifdef FIONBIO
static int s_nbio = 0;
#endif

int thread_number = 10;
int number_of_loops = 10;
int reconnect = 0;
int cache_stats = 0;

static const char rnd_seed[] =
    "string to make the random number generator think it has entropy";

int doit(char *ctx[4]);
static void print_stats(BIO *bio, SSL_CTX *ctx)
{
    BIO_printf(bio, "%4ld items in the session cache\n",
	       SSL_CTX_sess_number(ctx));
    BIO_printf(bio, "%4d client connects (SSL_connect())\n",
	       SSL_CTX_sess_connect(ctx));
    BIO_printf(bio, "%4d client connects that finished\n",
	       SSL_CTX_sess_connect_good(ctx));
    BIO_printf(bio, "%4d server connects (SSL_accept())\n",
	       SSL_CTX_sess_accept(ctx));
    BIO_printf(bio, "%4d server connects that finished\n",
	       SSL_CTX_sess_accept_good(ctx));
    BIO_printf(bio, "%4d session cache hits\n", SSL_CTX_sess_hits(ctx));
    BIO_printf(bio, "%4d session cache misses\n", SSL_CTX_sess_misses(ctx));
    BIO_printf(bio, "%4d session cache timeouts\n", SSL_CTX_sess_timeouts(ctx));
}

static void sv_usage(void)
{
    BIO_printf(bio_err, "usage: ssltest [args ...]\n");
    BIO_printf(bio_err, "\n");
    BIO_printf(bio_err, " -server_auth  - check server certificate\n");
    BIO_printf(bio_err, " -client_auth  - do client authentication\n");
    BIO_printf(bio_err, " -v            - more output\n");
    BIO_printf(bio_err, " -CApath arg   - PEM format directory of CA's\n");
    BIO_printf(bio_err, " -CAfile arg   - PEM format file of CA's\n");
    BIO_printf(bio_err, " -threads arg  - number of threads\n");
    BIO_printf(bio_err, " -loops arg    - number of 'connections', per thread\n");
    BIO_printf(bio_err, " -reconnect    - reuse session-id's\n");
    BIO_printf(bio_err, " -stats        - server session-id cache stats\n");
    BIO_printf(bio_err, " -cert arg     - server certificate/key\n");
    BIO_printf(bio_err, " -ccert arg    - client certificate/key\n");
    BIO_printf(bio_err, " -ssl3         - just SSLv3n\n");
}

int main(int argc, char *argv[])
{
    char *CApath = NULL, *CAfile = NULL;
    int badop = 0;
    int ret = 1;
    int client_auth = 0;
    int server_auth = 0;
    SSL_CTX *s_ctx = NULL;
    SSL_CTX *c_ctx = NULL;
    char *scert = TEST_SERVER_CERT;
    char *ccert = TEST_CLIENT_CERT;
    const SSL_METHOD *ssl_method = TLS_method();

    RAND_seed(rnd_seed, sizeof rnd_seed);

    if (bio_err == NULL)
        bio_err = BIO_new_fd(2, BIO_NOCLOSE);
    if (bio_stdout == NULL)
        bio_stdout = BIO_new_fd(1, BIO_NOCLOSE);
    argc--;
    argv++;

    while (argc >= 1) {
        if (strcmp(*argv, "-server_auth") == 0)
            server_auth = 1;
        else if (strcmp(*argv, "-client_auth") == 0)
            client_auth = 1;
        else if (strcmp(*argv, "-reconnect") == 0)
            reconnect = 1;
        else if (strcmp(*argv, "-stats") == 0)
            cache_stats = 1;
        else if (strcmp(*argv, "-ssl3") == 0)
            ssl_method = SSLv3_method();
        else if (strcmp(*argv, "-CApath") == 0) {
            if (--argc < 1)
                goto bad;
            CApath = *(++argv);
        } else if (strcmp(*argv, "-CAfile") == 0) {
            if (--argc < 1)
                goto bad;
            CAfile = *(++argv);
        } else if (strcmp(*argv, "-cert") == 0) {
            if (--argc < 1)
                goto bad;
            scert = *(++argv);
        } else if (strcmp(*argv, "-ccert") == 0) {
            if (--argc < 1)
                goto bad;
            ccert = *(++argv);
        } else if (strcmp(*argv, "-threads") == 0) {
            if (--argc < 1)
                goto bad;
            thread_number = atoi(*(++argv));
            if (thread_number == 0)
                thread_number = 1;
            if (thread_number > MAX_THREAD_NUMBER)
                thread_number = MAX_THREAD_NUMBER;
        } else if (strcmp(*argv, "-loops") == 0) {
            if (--argc < 1)
                goto bad;
            number_of_loops = atoi(*(++argv));
            if (number_of_loops == 0)
                number_of_loops = 1;
        } else {
            BIO_printf(bio_err, "unknown option %s\n", *argv);
            badop = 1;
            break;
        }
        argc--;
        argv++;
    }
    if (badop) {
 bad:
        sv_usage();
        goto end;
    }

    if (cipher == NULL && OPENSSL_issetugid() == 0)
        cipher = getenv("SSL_CIPHER");

    SSL_load_error_strings();
    OpenSSL_add_ssl_algorithms();

    c_ctx = SSL_CTX_new(ssl_method);
    s_ctx = SSL_CTX_new(ssl_method);
    if ((c_ctx == NULL) || (s_ctx == NULL)) {
        ERR_print_errors(bio_err);
        goto end;
    }

    SSL_CTX_set_session_cache_mode(s_ctx,
                                   SSL_SESS_CACHE_NO_AUTO_CLEAR |
                                   SSL_SESS_CACHE_SERVER);
    SSL_CTX_set_session_cache_mode(c_ctx,
                                   SSL_SESS_CACHE_NO_AUTO_CLEAR |
                                   SSL_SESS_CACHE_SERVER);

    if (!SSL_CTX_use_certificate_file(s_ctx, scert, SSL_FILETYPE_PEM)) {
        BIO_printf(bio_err, "SSL_CTX_use_certificate_file (%s)\n", scert);
        ERR_print_errors(bio_err);
        goto end;
    } else
        if (!SSL_CTX_use_RSAPrivateKey_file(s_ctx, scert, SSL_FILETYPE_PEM)) {
        BIO_printf(bio_err, "SSL_CTX_use_RSAPrivateKey_file (%s)\n", scert);
        ERR_print_errors(bio_err);
        goto end;
    }

    if (client_auth) {
        SSL_CTX_use_certificate_file(c_ctx, ccert, SSL_FILETYPE_PEM);
        SSL_CTX_use_RSAPrivateKey_file(c_ctx, ccert, SSL_FILETYPE_PEM);
    }

    if ((!SSL_CTX_load_verify_locations(s_ctx, CAfile, CApath)) ||
        (!SSL_CTX_set_default_verify_paths(s_ctx)) ||
        (!SSL_CTX_load_verify_locations(c_ctx, CAfile, CApath)) ||
        (!SSL_CTX_set_default_verify_paths(c_ctx))) {
        BIO_printf(bio_err, "SSL_load_verify_locations\n");
        ERR_print_errors(bio_err);
        goto end;
    }

    if (client_auth) {
        BIO_printf(bio_err, "client authentication\n");
        SSL_CTX_set_verify(s_ctx,
                           SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT,
                           verify_callback);
    }
    if (server_auth) {
        BIO_printf(bio_err, "server authentication\n");
        SSL_CTX_set_verify(c_ctx, SSL_VERIFY_PEER, verify_callback);
    }

    thread_setup();
    do_threads(s_ctx, c_ctx);
    thread_cleanup();
 end:

    if (c_ctx != NULL) {
        BIO_printf(bio_err, "Client SSL_CTX stats then free it\n");
        print_stats(bio_err, c_ctx);
        SSL_CTX_free(c_ctx);
    }
    if (s_ctx != NULL) {
        BIO_printf(bio_err, "Server SSL_CTX stats then free it\n");
        print_stats(bio_err, s_ctx);
        if (cache_stats) {
            BIO_printf(bio_err, "-----\n");
            lh_SSL_SESSION_stats_bio(SSL_CTX_sessions(s_ctx), bio_err);
            BIO_printf(bio_err, "-----\n");
    /*-     lh_SSL_SESSION_node_stats_bio(SSL_CTX_sessions(s_ctx),bio_err);
            BIO_printf(bio_err,"-----\n"); */
            lh_SSL_SESSION_node_usage_stats_bio(SSL_CTX_sessions(s_ctx), bio_err);
            BIO_printf(bio_err, "-----\n");
        }
        SSL_CTX_free(s_ctx);
        BIO_printf(bio_err, "done free\n");
    }
    exit(ret);
    return (0);
}

#define W_READ  1
#define W_WRITE 2
#define C_DONE  1
#define S_DONE  2

int ndoit(SSL_CTX *ssl_ctx[2])
{
    int i;
    int ret;
    char *ctx[4];
    CRYPTO_THREADID thread_id;

    ctx[0] = (char *)ssl_ctx[0];
    ctx[1] = (char *)ssl_ctx[1];

    if (reconnect) {
        ctx[2] = (char *)SSL_new(ssl_ctx[0]);
        ctx[3] = (char *)SSL_new(ssl_ctx[1]);
    } else {
        ctx[2] = NULL;
        ctx[3] = NULL;
    }

    CRYPTO_THREADID_current(&thread_id);
    BIO_printf(bio_stdout, "started thread %lu\n",
	       CRYPTO_THREADID_hash(&thread_id));
    for (i = 0; i < number_of_loops; i++) {
/*-     BIO_printf(bio_err,"%4d %2d ctx->ref (%3d,%3d)\n",
                   CRYPTO_THREADID_hash(&thread_id),i,
                   ssl_ctx[0]->references,
                   ssl_ctx[1]->references); */
/*      pthread_delay_np(&tm); */

        ret = doit(ctx);
        if (ret != 0) {
            BIO_printf(bio_stdout, "error[%d] %lu - %d\n",
                       i, CRYPTO_THREADID_hash(&thread_id), ret);
            return (ret);
        }
    }
    BIO_printf(bio_stdout, "DONE %lu\n", CRYPTO_THREADID_hash(&thread_id));
    if (reconnect) {
        SSL_free((SSL *)ctx[2]);
        SSL_free((SSL *)ctx[3]);
    }
    return (0);
}

int doit(char *ctx[4])
{
    SSL_CTX *s_ctx, *c_ctx;
    static char cbuf[200], sbuf[200];
    SSL *c_ssl = NULL;
    SSL *s_ssl = NULL;
    BIO *c_to_s = NULL;
    BIO *s_to_c = NULL;
    BIO *c_bio = NULL;
    BIO *s_bio = NULL;
    int c_r, c_w, s_r, s_w;
    int c_want, s_want;
    int i;
    int done = 0;
    int c_write, s_write;
    int do_server = 0, do_client = 0;

    s_ctx = (SSL_CTX *)ctx[0];
    c_ctx = (SSL_CTX *)ctx[1];

    if (ctx[2] != NULL)
        s_ssl = (SSL *)ctx[2];
    else
        s_ssl = SSL_new(s_ctx);

    if (ctx[3] != NULL)
        c_ssl = (SSL *)ctx[3];
    else
        c_ssl = SSL_new(c_ctx);

    if ((s_ssl == NULL) || (c_ssl == NULL))
        goto err;

    c_to_s = BIO_new(BIO_s_mem());
    s_to_c = BIO_new(BIO_s_mem());
    if ((s_to_c == NULL) || (c_to_s == NULL))
        goto err;

    c_bio = BIO_new(BIO_f_ssl());
    s_bio = BIO_new(BIO_f_ssl());
    if ((c_bio == NULL) || (s_bio == NULL))
        goto err;

    SSL_set_connect_state(c_ssl);
    SSL_set_bio(c_ssl, s_to_c, c_to_s);
    BIO_set_ssl(c_bio, c_ssl, (ctx[2] == NULL) ? BIO_CLOSE : BIO_NOCLOSE);

    SSL_set_accept_state(s_ssl);
    SSL_set_bio(s_ssl, c_to_s, s_to_c);
    BIO_set_ssl(s_bio, s_ssl, (ctx[3] == NULL) ? BIO_CLOSE : BIO_NOCLOSE);

    c_r = 0;
    s_r = 1;
    c_w = 1;
    s_w = 0;
    c_want = W_WRITE;
    s_want = 0;
    c_write = 1, s_write = 0;

    /* We can always do writes */
    for (;;) {
        do_server = 0;
        do_client = 0;

        i = (int)BIO_pending(s_bio);
        if ((i && s_r) || s_w)
            do_server = 1;

        i = (int)BIO_pending(c_bio);
        if ((i && c_r) || c_w)
            do_client = 1;

        if (do_server && verbose) {
            if (SSL_in_init(s_ssl))
                BIO_printf(bio_stdout, "server waiting in SSL_accept - %s\n",
                           SSL_state_string_long(s_ssl));
            else if (s_write)
                BIO_printf(bio_stdout, "server:SSL_write()\n");
            else
                BIO_printf(bio_stdout, "server:SSL_read()\n");
        }

        if (do_client && verbose) {
            if (SSL_in_init(c_ssl))
                BIO_printf(bio_stdout, "client waiting in SSL_connect - %s\n",
                           SSL_state_string_long(c_ssl));
            else if (c_write)
                BIO_printf(bio_stdout, "client:SSL_write()\n");
            else
                BIO_printf(bio_stdout, "client:SSL_read()\n");
        }

        if (!do_client && !do_server) {
            BIO_printf(bio_stdout, "ERROR IN STARTUP\n");
            break;
        }
        if (do_client && !(done & C_DONE)) {
            if (c_write) {
                i = BIO_write(c_bio, "hello from client\n", 18);
                if (i < 0) {
                    c_r = 0;
                    c_w = 0;
                    if (BIO_should_retry(c_bio)) {
                        if (BIO_should_read(c_bio))
                            c_r = 1;
                        if (BIO_should_write(c_bio))
                            c_w = 1;
                    } else {
                        BIO_printf(bio_err, "ERROR in CLIENT\n");
                        ERR_print_errors_fp(stderr);
                        return (1);
                    }
                } else if (i == 0) {
                    BIO_printf(bio_err, "SSL CLIENT STARTUP FAILED\n");
                    return (1);
                } else {
                    /* ok */
                    c_write = 0;
                }
            } else {
                i = BIO_read(c_bio, cbuf, 100);
                if (i < 0) {
                    c_r = 0;
                    c_w = 0;
                    if (BIO_should_retry(c_bio)) {
                        if (BIO_should_read(c_bio))
                            c_r = 1;
                        if (BIO_should_write(c_bio))
                            c_w = 1;
                    } else {
                        BIO_printf(bio_err, "ERROR in CLIENT\n");
                        ERR_print_errors_fp(stderr);
                        return (1);
                    }
                } else if (i == 0) {
                    BIO_printf(bio_err, "SSL CLIENT STARTUP FAILED\n");
                    return (1);
                } else {
                    done |= C_DONE;
                }
            }
        }

        if (do_server && !(done & S_DONE)) {
            if (!s_write) {
                i = BIO_read(s_bio, sbuf, 100);
                if (i < 0) {
                    s_r = 0;
                    s_w = 0;
                    if (BIO_should_retry(s_bio)) {
                        if (BIO_should_read(s_bio))
                            s_r = 1;
                        if (BIO_should_write(s_bio))
                            s_w = 1;
                    } else {
                        BIO_printf(bio_err, "ERROR in SERVER\n");
                        ERR_print_errors_fp(stderr);
                        return (1);
                    }
                } else if (i == 0) {
                    BIO_printf(bio_err, "SSL SERVER STARTUP FAILED\n");
                    return (1);
                } else {
                    s_write = 1;
                    s_w = 1;
                }
            } else {
                i = BIO_write(s_bio, "hello from server\n", 18);
                if (i < 0) {
                    s_r = 0;
                    s_w = 0;
                    if (BIO_should_retry(s_bio)) {
                        if (BIO_should_read(s_bio))
                            s_r = 1;
                        if (BIO_should_write(s_bio))
                            s_w = 1;
                    } else {
                        BIO_printf(bio_err, "ERROR in SERVER\n");
                        ERR_print_errors_fp(stderr);
                        return (1);
                    }
                } else if (i == 0) {
                    BIO_printf(bio_err, "SSL SERVER STARTUP FAILED\n");
                    return (1);
                } else {
                    s_write = 0;
                    s_r = 1;
                    done |= S_DONE;
                }
            }
        }

        if ((done & S_DONE) && (done & C_DONE))
            break;
    }

    SSL_set_shutdown(c_ssl, SSL_SENT_SHUTDOWN | SSL_RECEIVED_SHUTDOWN);
    SSL_set_shutdown(s_ssl, SSL_SENT_SHUTDOWN | SSL_RECEIVED_SHUTDOWN);

 err:
#if 0
    /*
     * We have to set the BIO's to NULL otherwise they will be free()ed
     * twice.  Once when th s_ssl is SSL_free()ed and again when c_ssl is
     * SSL_free()ed. This is a hack required because s_ssl and c_ssl are
     * sharing the same BIO structure and SSL_set_bio() and SSL_free()
     * automatically BIO_free non NULL entries. You should not normally do
     * this or be required to do this
     */

    if (s_ssl != NULL) {
        s_ssl->rbio = NULL;
        s_ssl->wbio = NULL;
    }
    if (c_ssl != NULL) {
        c_ssl->rbio = NULL;
        c_ssl->wbio = NULL;
    }

    /* The SSL's are optionally freed in the following calls */
    BIO_free(c_to_s);
    BIO_free(s_to_c);
#endif

    BIO_free(c_bio);
    BIO_free(s_bio);
    return (0);
}

int verify_callback(int ok, X509_STORE_CTX *ctx)
{
    char *s, buf[256];

    if (verbose) {
        s = X509_NAME_oneline(X509_get_subject_name(ctx->current_cert),
                              buf, 256);
        if (s != NULL) {
            if (ok)
                BIO_printf(bio_err, "depth=%d %s\n", ctx->error_depth, buf);
            else
                BIO_printf(bio_err, "depth=%d error=%d %s\n",
                        ctx->error_depth, ctx->error, buf);
        }
    }
    return (ok);
}

#define THREAD_STACK_SIZE (16*1024)

#ifdef OPENSSL_SYS_WIN32

static HANDLE *lock_cs;

void thread_setup(void)
{
    int i;

    lock_cs = OPENSSL_malloc(CRYPTO_num_locks() * sizeof(HANDLE));
    for (i = 0; i < CRYPTO_num_locks(); i++) {
        lock_cs[i] = CreateMutex(NULL, FALSE, NULL);
    }

    CRYPTO_set_locking_callback((void (*)(int, int, char *, int))
                                win32_locking_callback);
    /* id callback defined */
}

void thread_cleanup(void)
{
    int i;

    CRYPTO_set_locking_callback(NULL);
    for (i = 0; i < CRYPTO_num_locks(); i++)
        CloseHandle(lock_cs[i]);
    OPENSSL_free(lock_cs);
}

void win32_locking_callback(int mode, int type, const char *file, int line)
{
    if (mode & CRYPTO_LOCK) {
        WaitForSingleObject(lock_cs[type], INFINITE);
    } else {
        ReleaseMutex(lock_cs[type]);
    }
}

void do_threads(SSL_CTX *s_ctx, SSL_CTX *c_ctx)
{
    double ret;
    SSL_CTX *ssl_ctx[2];
    DWORD thread_id[MAX_THREAD_NUMBER];
    HANDLE thread_handle[MAX_THREAD_NUMBER];
    int i;
    SYSTEMTIME start, end;

    ssl_ctx[0] = s_ctx;
    ssl_ctx[1] = c_ctx;

    GetSystemTime(&start);
    for (i = 0; i < thread_number; i++) {
        thread_handle[i] = CreateThread(NULL,
                                        THREAD_STACK_SIZE,
                                        (LPTHREAD_START_ROUTINE) ndoit,
                                        (void *)ssl_ctx, 0L, &(thread_id[i]));
    }

    BIO_printf(bio_stdout, "reaping\n");
    for (i = 0; i < thread_number; i += 50) {
        int j;

        j = (thread_number < (i + 50)) ? (thread_number - i) : 50;

        if (WaitForMultipleObjects(j,
                                   (CONST HANDLE *) & (thread_handle[i]),
                                   TRUE, INFINITE)
            == WAIT_FAILED) {
            BIO_printf(bio_err, "WaitForMultipleObjects failed:%d\n",
                    GetLastError());
            exit(1);
        }
    }
    GetSystemTime(&end);

    if (start.wDayOfWeek > end.wDayOfWeek)
        end.wDayOfWeek += 7;
    ret = (end.wDayOfWeek - start.wDayOfWeek) * 24;

    ret = (ret + end.wHour - start.wHour) * 60;
    ret = (ret + end.wMinute - start.wMinute) * 60;
    ret = (ret + end.wSecond - start.wSecond);
    ret += (end.wMilliseconds - start.wMilliseconds) / 1000.0;

    BIO_printf(bio_stdout, "win32 threads done - %.3f seconds\n", ret);
}

#endif                          /* OPENSSL_SYS_WIN32 */


#ifdef PTHREADS

static pthread_mutex_t *lock_cs;
static long *lock_count;

void thread_setup(void)
{
    int i;

    lock_cs = OPENSSL_malloc(CRYPTO_num_locks() * sizeof(pthread_mutex_t));
    lock_count = OPENSSL_malloc(CRYPTO_num_locks() * sizeof(long));
    for (i = 0; i < CRYPTO_num_locks(); i++) {
        lock_count[i] = 0;
        pthread_mutex_init(&(lock_cs[i]), NULL);
    }

    CRYPTO_THREADID_set_callback(pthreads_thread_id);
    CRYPTO_set_locking_callback(pthreads_locking_callback);
}

void thread_cleanup(void)
{
    int i;

    CRYPTO_set_locking_callback(NULL);
    BIO_printf(bio_err, "cleanup\n");
    for (i = 0; i < CRYPTO_num_locks(); i++) {
        pthread_mutex_destroy(&(lock_cs[i]));
        BIO_printf(bio_err, "%8ld:%s\n", lock_count[i], CRYPTO_get_lock_name(i));
    }
    OPENSSL_free(lock_cs);
    OPENSSL_free(lock_count);

    BIO_printf(bio_err, "done cleanup\n");
}

void pthreads_locking_callback(int mode, int type, const char *file, int line)
{
    if (mode & CRYPTO_LOCK) {
        pthread_mutex_lock(&(lock_cs[type]));
        lock_count[type]++;
    } else {
        pthread_mutex_unlock(&(lock_cs[type]));
    }
}

void do_threads(SSL_CTX *s_ctx, SSL_CTX *c_ctx)
{
    SSL_CTX *ssl_ctx[2];
    pthread_t thread_ctx[MAX_THREAD_NUMBER];
    int i;

    ssl_ctx[0] = s_ctx;
    ssl_ctx[1] = c_ctx;

    for (i = 0; i < thread_number; i++) {
        pthread_create(&(thread_ctx[i]), NULL,
                       (void *(*)())ndoit, (void *)ssl_ctx);
    }

    BIO_printf(bio_stdout, "reaping\n");
    for (i = 0; i < thread_number; i++) {
        pthread_join(thread_ctx[i], NULL);
    }

#if 0 /* We can't currently find out the reference amount */
    BIO_printf(bio_stdout, "pthreads threads done (%d,%d)\n",
               s_ctx->references, c_ctx->references);
#else
    BIO_printf(bio_stdout, "pthreads threads done\n");
#endif
}

void pthreads_thread_id(CRYPTO_THREADID *tid)
{
    CRYPTO_THREADID_set_numeric(tid, (unsigned long)pthread_self());
}

#endif                          /* PTHREADS */
