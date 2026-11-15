/*
 * Reproduces the GeForceGLDriverWeb 334.01.03f11 query report buffer fault
 * and checks query results.
 *
 * Issues N occlusion queries (default 32) inside one GL_TIME_ELAPSED query.
 * Query i draws a quad clipped by a scissor rectangle of
 * (i % 16 + 1) x (i % 8 + 1) pixels, so its exact result is known.  Once
 * more than 31 occlusion queries have been created inside the timer query,
 * Apple's query report buffer grows; the stock driver then writes the
 * timer query's result into the destroyed buffer, the GPU channel faults
 * (MMU Error 2) and the result never arrives.  With the fix the timer query
 * resolves and every occlusion result is exact.
 *
 * The GL work runs in a forked child because the driver aborts the process
 * when its channel faults.  The parent reports:
 *   exit 0  no fault, all results correct
 *   exit 1  fault reproduced (the child aborted, stalled waiting for a
 *           result, or the kernel logged a channel exception)
 *   exit 2  no fault, but wrong query results
 *   exit 3  setup error
 *
 * usage: repro [count]     (31 or fewer does not fault the stock driver)
 * Build: clang -O2 -Wall -Wno-deprecated-declarations -o repro repro.c -framework OpenGL
 */
#include <OpenGL/OpenGL.h>
#include <OpenGL/gl.h>
#include <OpenGL/glext.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/wait.h>

#define LOG "/var/log/system.log"

static double now(void)
{
    struct timeval tv; gettimeofday(&tv, NULL);
    return tv.tv_sec + tv.tv_usec / 1e6;
}

/* Child: returns 0 (results correct), 2 (wrong results), 3 (setup error)
 * or 4 (stalled); faults abort the process. */
static int run(int n)
{
    CGLPixelFormatAttribute attrs[] = { kCGLPFAAccelerated, kCGLPFANoRecovery, 0 };
    CGLPixelFormatObj pf = NULL; GLint npf; CGLContextObj ctx = NULL;
    if (CGLChoosePixelFormat(attrs, &pf, &npf) || !pf || CGLCreateContext(pf, NULL, &ctx)) return 3;
    CGLSetCurrentContext(ctx);
    printf("%s, %s\n", glGetString(GL_RENDERER), glGetString(GL_VERSION));

    /* Queries need a bound framebuffer to do any GPU work. */
    GLuint fbo, rb;
    glGenFramebuffersEXT(1, &fbo);
    glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, fbo);
    glGenRenderbuffersEXT(1, &rb);
    glBindRenderbufferEXT(GL_RENDERBUFFER_EXT, rb);
    glRenderbufferStorageEXT(GL_RENDERBUFFER_EXT, GL_RGBA8, 64, 64);
    glFramebufferRenderbufferEXT(GL_FRAMEBUFFER_EXT, GL_COLOR_ATTACHMENT0_EXT, GL_RENDERBUFFER_EXT, rb);
    if (glCheckFramebufferStatusEXT(GL_FRAMEBUFFER_EXT) != GL_FRAMEBUFFER_COMPLETE_EXT) return 3;
    glViewport(0, 0, 64, 64);
    glEnable(GL_SCISSOR_TEST);

    double t0 = now();
    GLuint timer, *q = calloc(n, sizeof *q);
    glGenQueries(1, &timer);
    glBeginQuery(GL_TIME_ELAPSED_EXT, timer);
    for (int i = 0; i < n; i++) {
        glGenQueries(1, &q[i]);
        glScissor(0, 0, i % 16 + 1, i % 8 + 1);
        glBeginQuery(GL_SAMPLES_PASSED, q[i]);
        glBegin(GL_QUADS);
        glVertex2f(-1, -1); glVertex2f(1, -1); glVertex2f(1, 1); glVertex2f(-1, 1);
        glEnd();
        glEndQuery(GL_SAMPLES_PASSED);
    }
    glEndQuery(GL_TIME_ELAPSED_EXT);
    GLuint64EXT ns = 0;
    glGetQueryObjectui64vEXT(timer, GL_QUERY_RESULT, &ns);
    double dt = now() - t0;
    printf("%d occlusion queries inside a timer query: timer result %llu ns after %.3f s\n",
           n, (unsigned long long)ns, dt);
    if (dt > 2.0) {
        /* A dead channel never delivers the result; the driver gives up
         * after about 6 s, and the next submission reports the fault. */
        glFinish();
        return 4;
    }

    int wrong = 0;
    for (int i = 0; i < n; i++) {
        GLuint samples = 0, expect = (GLuint)((i % 16 + 1) * (i % 8 + 1));
        glGetQueryObjectuiv(q[i], GL_QUERY_RESULT, &samples);
        if (samples != expect) {
            if (wrong < 10) printf("query %d: %u samples, expected %u\n", i, samples, expect);
            wrong++;
        }
    }
    printf("occlusion results: %d of %d wrong\n", wrong, n);
    glFinish();
    CGLSetCurrentContext(NULL);
    CGLDestroyContext(ctx);
    return wrong ? 2 : 0;
}

static off_t logSize(void)
{
    struct stat st;
    return stat(LOG, &st) == 0 ? st.st_size : -1;
}

int main(int argc, char **argv)
{
    int n = argc > 1 ? atoi(argv[1]) : 32;
    if (n < 1) n = 1;
    fflush(stdout);

    off_t start = logSize();
    pid_t pid = fork();
    if (pid < 0) return 3;
    if (pid == 0) { int r = run(n); fflush(stdout); _exit(r); }
    int status;
    waitpid(pid, &status, 0);
    sleep(3);   /* the kernel logs the exception when the dead channel is next used */

    char line[1024], type[128] = "";
    FILE *f = fopen(LOG, "r");
    if (f && start >= 0) {
        fseeko(f, start, SEEK_SET);
        while (fgets(line, sizeof line, f)) {
            char *p = strstr(line, "Exception type = ");
            if (strstr(line, "Channel exception") && p && !*type) {
                strlcpy(type, p + 17, sizeof type);
                type[strcspn(type, "\n")] = 0;
            }
        }
    }
    if (f) fclose(f);

    int code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    if (code == 3) { printf("RESULT: setup error\n"); return 3; }
    if (WIFSIGNALED(status) || code == 4 || *type) {
        printf("RESULT: GPU FAULT REPRODUCED (%s)\n",
               *type ? type : WIFSIGNALED(status) ? strsignal(WTERMSIG(status)) : "query result never arrived");
        return 1;
    }
    if (code == 2) { printf("RESULT: no fault, but wrong query results\n"); return 2; }
    printf("RESULT: no fault, all query results correct\n");
    return 0;
}
