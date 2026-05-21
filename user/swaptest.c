#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

#define PGSIZE 4096

void die(char *msg)
{
    printf("FAIL: %s\n", msg);
    exit(1);
}

void check_direct_swapio(void)
{
    char *buf;
    char *out;
    int r1, w1, r2, w2;
    int i;

    printf("[1] direct swapread/swapwrite test\n");

    buf = sbrk(PGSIZE);
    out = sbrk(PGSIZE);
    if (buf == (char *)-1 || out == (char *)-1)
        die("sbrk failed");

    for (i = 0; i < PGSIZE; i++)
        buf[i] = (char)(i % 251);

    for (i = 0; i < PGSIZE; i++)
        out[i] = 0;

    swapstat(&r1, &w1);

    /*
     * swap slot 0에 직접 write/read한다.
     * 이 테스트는 page replacement로 swap slot이 사용되기 전에만 수행한다.
     */
    swapwrite(buf, 0);
    swapread(out, 0);

    swapstat(&r2, &w2);

    for (i = 0; i < PGSIZE; i++)
    {
        if (out[i] != (char)(i % 251))
            die("direct swap data mismatch");
    }

    if (r2 <= r1)
        die("swapread count did not increase");
    if (w2 <= w1)
        die("swapwrite count did not increase");

    printf("PASS: direct swap I/O, read %d->%d write %d->%d\n", r1, r2, w1, w2);
}

void check_pressure_and_content(void)
{
    int pages = 8000;
    char *base;
    int i;
    int r1, w1, r2, w2, r3, w3;

    printf("[2] swap pressure + content preservation test\n");

    swapstat(&r1, &w1);

    base = sbrk(pages * PGSIZE);
    if (base == (char *)-1)
        die("sbrk failed");

    /*
     * 모든 page를 touch한다.
     * PHYSTOP을 줄인 상태라면 여기서 kalloc 부족으로 swap-out이 발생할 수 있다.
     */
    for (i = 0; i < pages; i++)
    {
        base[i * PGSIZE] = (char)(i + 1);
        base[i * PGSIZE + 123] = (char)(i + 7);
    }

    swapstat(&r2, &w2);

    /*
     * 다시 앞 page부터 읽는다.
     * 일부 page가 swap-out되어 있었다면 여기서 page fault 후 swap-in된다.
     */
    for (i = 0; i < pages; i++)
    {
        if (base[i * PGSIZE] != (char)(i + 1))
            die("content mismatch at page start");
        if (base[i * PGSIZE + 123] != (char)(i + 7))
            die("content mismatch at page offset");
    }

    swapstat(&r3, &w3);

    printf("stats: read %d->%d->%d write %d->%d->%d\n", r1, r2, r3, w1, w2, w3);

    if (w3 == w1)
        printf("WARN: no swapwrite observed. This is OK only with normal PHYSTOP.\n");
    else
        printf("PASS: swapwrite increased\n");

    if (r3 == r2)
        printf("WARN: no swapread observed. This is OK only if pages were not swapped out before re-read.\n");
    else
        printf("PASS: swapread increased\n");

    printf("PASS: content preservation\n");
}

void check_fork_uvmcopy(void)
{
    int pages = 2000;
    char *base;
    int i;
    int pid;

    printf("[3] fork/uvmcopy test\n");

    base = sbrk(pages * PGSIZE);
    if (base == (char *)-1)
        die("sbrk failed");

    for (i = 0; i < pages; i++)
    {
        base[i * PGSIZE] = (char)(100 + i);
        base[i * PGSIZE + 77] = (char)(50 + i);
    }

    /*
     * parent의 일부 page가 swapped-out 상태일 수 있다.
     * fork 시 uvmcopy()가 swapped-out page도 제대로 child에게 복사해야 한다.
     */
    pid = fork();
    if (pid < 0)
        die("fork failed");

    if (pid == 0)
    {
        for (i = 0; i < pages; i++)
        {
            if (base[i * PGSIZE] != (char)(100 + i))
                die("child content mismatch at page start");
            if (base[i * PGSIZE + 77] != (char)(50 + i))
                die("child content mismatch at page offset");
        }

        /*
         * child가 자기 메모리를 수정해도 parent에는 영향이 없어야 한다.
         */
        for (i = 0; i < pages; i++)
            base[i * PGSIZE] = (char)i;

        printf("PASS: child verified copied memory\n");
        exit(0);
    }

    wait(0);

    for (i = 0; i < pages; i++)
    {
        if (base[i * PGSIZE] != (char)(100 + i))
            die("parent memory changed after child write");
    }

    printf("PASS: fork/uvmcopy\n");
}

void check_uvmunmap_sbrk_shrink(void)
{
    int pages = 4000;
    char *base;
    char *again;
    int i;
    int r1, w1, r2, w2;

    printf("[4] uvmunmap/sbrk shrink test\n");

    base = sbrk(pages * PGSIZE);
    if (base == (char *)-1)
        die("first sbrk failed");

    for (i = 0; i < pages; i++)
        base[i * PGSIZE] = (char)(i + 3);

    swapstat(&r1, &w1);

    /*
     * resident page는 kfree,
     * swapped-out page는 swap bitmap clear + PTE clear 되어야 한다.
     */
    if (sbrk(-pages * PGSIZE) == (char *)-1)
        die("sbrk shrink failed");

    again = sbrk(pages * PGSIZE);
    if (again == (char *)-1)
        die("second sbrk failed");

    for (i = 0; i < pages; i++)
        again[i * PGSIZE] = (char)(i + 9);

    for (i = 0; i < pages; i++)
    {
        if (again[i * PGSIZE] != (char)(i + 9))
            die("reallocated memory mismatch");
    }

    swapstat(&r2, &w2);

    printf("stats after shrink/realloc: read %d->%d write %d->%d\n", r1, r2, w1, w2);
    printf("PASS: uvmunmap/sbrk shrink\n");
}

int main(int argc, char *argv[])
{
    if (argc < 2)
    {
        printf("usage: swaptest direct|pressure|fork|unmap|all\n");
        exit(1);
    }

    if (strcmp(argv[1], "direct") == 0)
    {
        check_direct_swapio();
    }
    else if (strcmp(argv[1], "pressure") == 0)
    {
        check_pressure_and_content();
    }
    else if (strcmp(argv[1], "fork") == 0)
    {
        check_fork_uvmcopy();
    }
    else if (strcmp(argv[1], "unmap") == 0)
    {
        check_uvmunmap_sbrk_shrink();
    }
    else if (strcmp(argv[1], "all") == 0)
    {
        check_direct_swapio();
        check_pressure_and_content();
        check_fork_uvmcopy();
        check_uvmunmap_sbrk_shrink();
    }
    else
    {
        printf("unknown test: %s\n", argv[1]);
        exit(1);
    }

    printf("swaptest %s: OK\n", argv[1]);
    exit(0);
}