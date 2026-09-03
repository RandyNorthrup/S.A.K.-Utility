/*
 * Cppcheck language-routing fixture. The cast is ordinary C but a cstyleCast finding in C++, so
 * this clean translation unit fails if scripts/run_cppcheck.ps1 ever forces .c input through C++.
 */

int main(int argc, char** argv) {
    const void* raw = argv;
    const unsigned char* bytes = (const unsigned char*)raw;
    return bytes == 0 ? argc : bytes[0];
}
