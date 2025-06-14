#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#define MAX_INPUT 1024


int main() {
    char input[MAX_INPUT];

    for (;;) {
        printf("ash$ ");
        fflush(stdout);

        if (!fgets(input, MAX_INPUT, stdin))
            break;

        input[strcspn(input, "\n")] = 0;

        if (!strlen(input))
            continue;

        printf("'%s'\n", input);

        if (!strcmp(input, "exit"))
            break;
    }

    printf("\n...terminating ash\n");
    return 0;
}
