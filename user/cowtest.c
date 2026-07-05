#include <stdio.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/wait.h>

#define COW_PAGE_SIZE 4096

// COW を誘発するための大きな配列
static char basic_data[8192] = "Initial Data";
static char prot_page[COW_PAGE_SIZE] __attribute__((aligned(COW_PAGE_SIZE)));

static int test_basic_cow(void) {
    basic_data[0] = 'I';
    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        return 1;
    }
    if (pid == 0) {
        basic_data[0] = 'C';
        _exit(basic_data[0] == 'C' ? 0 : 1);
    }
    int status;
    waitpid(pid, &status, 0);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        printf("cowtest: basic child failed status=%d\n", status);
        return 1;
    }
    if (basic_data[0] != 'I') {
        printf("cowtest: basic child write leaked into parent\n");
        return 1;
    }
    return 0;
}

// fork 後の COW ページに mprotect(PROT_READ|PROT_WRITE) しても、共有中の
// 物理ページが書き込み可能になってはならない。子の書き込みは COW フォルト
// 経由で複製ページに入り、親側の値は変わらないことを検証する。
static int test_mprotect_cow(void) {
    prot_page[0] = 'X';
    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        return 1;
    }
    if (pid == 0) {
        if (mprotect(prot_page, COW_PAGE_SIZE, PROT_READ | PROT_WRITE) != 0) _exit(2);
        prot_page[0] = 'C';
        _exit(prot_page[0] == 'C' ? 0 : 1);
    }
    int status;
    waitpid(pid, &status, 0);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        printf("cowtest: mprotect child failed status=%d\n", status);
        return 1;
    }
    if (prot_page[0] != 'X') {
        printf("cowtest: mprotect child write leaked into parent\n");
        return 1;
    }
    return 0;
}

int main(void) {
    int fails = 0;
    fails += test_basic_cow();
    fails += test_mprotect_cow();
    if (fails) {
        printf("cowtest: FAIL\n");
        return 1;
    }
    printf("cowtest: PASS\n");
    return 0;
}
