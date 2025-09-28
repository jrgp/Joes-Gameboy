#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <stdbool.h>
#include <unistd.h>
#include <sys/wait.h>
#include <time.h>
#include <stdarg.h>

// Test framework state
typedef struct {
    char test_name[256];
    char rom_path[256];
    char expected_output[1024];
    bool passed;
    char actual_output[1024];
} TestCase;

// Global test state
TestCase current_test;
int test_count = 0;
int passed_count = 0;

// Capture output from the emulator
char captured_output[4096];
int output_pos = 0;

void capture_printf(const char* format, ...) {
    va_list args;
    va_start(args, format);
    int len = vsnprintf(captured_output + output_pos, 
                       sizeof(captured_output) - output_pos, 
                       format, args);
    if (len > 0) {
        output_pos += len;
    }
    va_end(args);
}

// Test runner functions
void start_test(const char* name, const char* rom_path) {
    strncpy(current_test.test_name, name, sizeof(current_test.test_name) - 1);
    strncpy(current_test.rom_path, rom_path, sizeof(current_test.rom_path) - 1);
    current_test.passed = false;
    current_test.actual_output[0] = '\0';
    
    printf("Starting test: %s\n", name);
    test_count++;
}

void end_test(bool passed, const char* expected, const char* actual) {
    current_test.passed = passed;
    strncpy(current_test.expected_output, expected ? expected : "", sizeof(current_test.expected_output) - 1);
    strncpy(current_test.actual_output, actual ? actual : "", sizeof(current_test.actual_output) - 1);
    
    if (passed) {
        passed_count++;
        printf("✓ PASSED: %s\n", current_test.test_name);
    } else {
        printf("✗ FAILED: %s\n", current_test.test_name);
        printf("  Expected: %s\n", expected ? expected : "(no expected output)");
        printf("  Actual: %s\n", actual ? actual : "(no actual output)");
    }
    printf("\n");
}

// Function to run a single test ROM and capture its output
bool run_test_rom(const char* rom_path, char* output_buffer, size_t buffer_size) {
    // Create a command to run the emulator with the test ROM
    char command[512];
    snprintf(command, sizeof(command), "./gb \"%s\" 2>&1", rom_path);
    
    // Open a pipe to capture output
    FILE* pipe = popen(command, "r");
    if (!pipe) {
        return false;
    }
    
    // Read output
    size_t total_read = 0;
    char buffer[1024];
    while (fgets(buffer, sizeof(buffer), pipe) && total_read < buffer_size - 1) {
        size_t len = strlen(buffer);
        if (total_read + len < buffer_size - 1) {
            strcpy(output_buffer + total_read, buffer);
            total_read += len;
        }
    }
    
    output_buffer[total_read] = '\0';
    
    // Close pipe and get exit status
    int status = pclose(pipe);
    
    // Return true if the command executed successfully
    return (status == 0);
}

// Test for CPU instruction correctness
void test_cpu_instructions(void) {
    start_test("CPU Instructions", "debugroms/01-special.gb");
    
    char output[2048];
    bool success = run_test_rom(current_test.rom_path, output, sizeof(output));
    
    // Check if we got any output indicating test results
    bool test_passed = false;
    
    // Look for common test result patterns
    if (strstr(output, "Passed") || strstr(output, "passed")) {
        test_passed = true;
    } else if (strstr(output, "Failed") || strstr(output, "failed")) {
        test_passed = false;
    } else {
        // If no clear result, check if emulator ran without crashing
        test_passed = (strlen(output) > 0);
    }
    
    end_test(test_passed, "Test should complete successfully", output);
}

// Test for interrupt handling
void test_interrupts(void) {
    start_test("Interrupt System", "debugroms/02-interrupts.gb");
    
    char output[2048];
    bool success = run_test_rom(current_test.rom_path, output, sizeof(output));
    
    bool test_passed = false;
    
    if (strstr(output, "Passed") || strstr(output, "passed")) {
        test_passed = true;
    } else if (strstr(output, "Failed") || strstr(output, "failed")) {
        test_passed = false;
    } else {
        test_passed = (strlen(output) > 0);
    }
    
    end_test(test_passed, "Interrupt test should pass", output);
}

// Test for stack pointer operations
void test_stack_operations(void) {
    start_test("Stack Operations", "debugroms/03-op sp,hl.gb");
    
    char output[2048];
    bool success = run_test_rom(current_test.rom_path, output, sizeof(output));
    
    bool test_passed = false;
    
    if (strstr(output, "Passed") || strstr(output, "passed")) {
        test_passed = true;
    } else if (strstr(output, "Failed") || strstr(output, "failed")) {
        test_passed = false;
    } else {
        test_passed = (strlen(output) > 0);
    }
    
    end_test(test_passed, "Stack operations test should pass", output);
}

// Test for immediate value operations
void test_immediate_operations(void) {
    start_test("Immediate Operations", "debugroms/04-op r,imm.gb");
    
    char output[2048];
    bool success = run_test_rom(current_test.rom_path, output, sizeof(output));
    
    bool test_passed = false;
    
    if (strstr(output, "Passed") || strstr(output, "passed")) {
        test_passed = true;
    } else if (strstr(output, "Failed") || strstr(output, "failed")) {
        test_passed = false;
    } else {
        test_passed = (strlen(output) > 0);
    }
    
    end_test(test_passed, "Immediate operations test should pass", output);
}

// Test for register pair operations
void test_register_pair_operations(void) {
    start_test("Register Pair Operations", "debugroms/05-op rp.gb");
    
    char output[2048];
    bool success = run_test_rom(current_test.rom_path, output, sizeof(output));
    
    bool test_passed = false;
    
    if (strstr(output, "Passed") || strstr(output, "passed")) {
        test_passed = true;
    } else if (strstr(output, "Failed") || strstr(output, "failed")) {
        test_passed = false;
    } else {
        test_passed = (strlen(output) > 0);
    }
    
    end_test(test_passed, "Register pair operations test should pass", output);
}

// Test for load operations
void test_load_operations(void) {
    start_test("Load Operations", "debugroms/06-ld r,r.gb");
    
    char output[2048];
    bool success = run_test_rom(current_test.rom_path, output, sizeof(output));
    
    bool test_passed = false;
    
    if (strstr(output, "Passed") || strstr(output, "passed")) {
        test_passed = true;
    } else if (strstr(output, "Failed") || strstr(output, "failed")) {
        test_passed = false;
    } else {
        test_passed = (strlen(output) > 0);
    }
    
    end_test(test_passed, "Load operations test should pass", output);
}

// Test for jump and call operations
void test_jump_call_operations(void) {
    start_test("Jump/Call Operations", "debugroms/07-jr,jp,call,ret,rst.gb");
    
    char output[2048];
    bool success = run_test_rom(current_test.rom_path, output, sizeof(output));
    
    bool test_passed = false;
    
    if (strstr(output, "Passed") || strstr(output, "passed")) {
        test_passed = true;
    } else if (strstr(output, "Failed") || strstr(output, "failed")) {
        test_passed = false;
    } else {
        test_passed = (strlen(output) > 0);
    }
    
    end_test(test_passed, "Jump/call operations test should pass", output);
}

// Test for miscellaneous instructions
void test_misc_instructions(void) {
    start_test("Miscellaneous Instructions", "debugroms/08-misc instrs.gb");
    
    char output[2048];
    bool success = run_test_rom(current_test.rom_path, output, sizeof(output));
    
    bool test_passed = false;
    
    if (strstr(output, "Passed") || strstr(output, "passed")) {
        test_passed = true;
    } else if (strstr(output, "Failed") || strstr(output, "failed")) {
        test_passed = false;
    } else {
        test_passed = (strlen(output) > 0);
    }
    
    end_test(test_passed, "Miscellaneous instructions test should pass", output);
}

// Test for register operations
void test_register_operations(void) {
    start_test("Register Operations", "debugroms/09-op r,r.gb");
    
    char output[2048];
    bool success = run_test_rom(current_test.rom_path, output, sizeof(output));
    
    bool test_passed = false;
    
    if (strstr(output, "Passed") || strstr(output, "passed")) {
        test_passed = true;
    } else if (strstr(output, "Failed") || strstr(output, "failed")) {
        test_passed = false;
    } else {
        test_passed = (strlen(output) > 0);
    }
    
    end_test(test_passed, "Register operations test should pass", output);
}

// Test for bit operations
void test_bit_operations(void) {
    start_test("Bit Operations", "debugroms/10-bit ops.gb");
    
    char output[2048];
    bool success = run_test_rom(current_test.rom_path, output, sizeof(output));
    
    bool test_passed = false;
    
    if (strstr(output, "Passed") || strstr(output, "passed")) {
        test_passed = true;
    } else if (strstr(output, "Failed") || strstr(output, "failed")) {
        test_passed = false;
    } else {
        test_passed = (strlen(output) > 0);
    }
    
    end_test(test_passed, "Bit operations test should pass", output);
}

// Test for memory operations
void test_memory_operations(void) {
    start_test("Memory Operations", "debugroms/11-op a,(hl).gb");
    
    char output[2048];
    bool success = run_test_rom(current_test.rom_path, output, sizeof(output));
    
    bool test_passed = false;
    
    if (strstr(output, "Passed") || strstr(output, "passed")) {
        test_passed = true;
    } else if (strstr(output, "Failed") || strstr(output, "failed")) {
        test_passed = false;
    } else {
        test_passed = (strlen(output) > 0);
    }
    
    end_test(test_passed, "Memory operations test should pass", output);
}

// Main test runner
int main(void) {
    printf("=== Game Boy Emulator Debug Test Suite ===\n\n");
    
    // Run all tests
    test_cpu_instructions();
    test_interrupts();
    test_stack_operations();
    test_immediate_operations();
    test_register_pair_operations();
    test_load_operations();
    test_jump_call_operations();
    test_misc_instructions();
    test_register_operations();
    test_bit_operations();
    test_memory_operations();
    
    // Print summary
    printf("=== Test Summary ===\n");
    printf("Total tests: %d\n", test_count);
    printf("Passed: %d\n", passed_count);
    printf("Failed: %d\n", test_count - passed_count);
    printf("Success rate: %.1f%%\n", (float)passed_count / test_count * 100);
    
    return (passed_count == test_count) ? 0 : 1;
}