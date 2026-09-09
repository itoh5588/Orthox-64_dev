#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

int main(int argc, char *argv[]) {
    // Set environment variables for Python
    setenv("PYTHONHOME", "/", 1);
    setenv("PYTHONPATH", "/lib/python3.12:/lib", 1);
    setenv("PYTHONPLATLIBDIR", "lib", 1);
    
    // Execute the actual Python binary
    execv("/bin/python3.12", argv);
    
    // If execv returns, it means it failed
    perror("execv failed");
    return 1;
}
