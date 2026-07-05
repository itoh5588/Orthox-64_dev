extern int write(int fd, const void* buf, int count);

int main(void) {
    char msg[13];
    msg[0] = 'H';
    msg[1] = 'e';
    msg[2] = 'l';
    msg[3] = 'l';
    msg[4] = 'o';
    msg[5] = ',';
    msg[6] = ' ';
    msg[7] = 'w';
    msg[8] = 'o';
    msg[9] = 'r';
    msg[10] = 'l';
    msg[11] = 'd';
    msg[12] = '\n';
    write(1, msg, sizeof(msg) - 1);
    return 0;
}
