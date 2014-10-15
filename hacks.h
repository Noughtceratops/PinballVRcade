struct rolling_crc {
    unsigned block_size;
    unsigned a;
    unsigned b;
};
void compute_fingerprint (rolling_crc* fingerprint, unsigned block_size, const unsigned char block[]);
uintptr_t find_fingerprint (const rolling_crc& fingerprint);
void read_code (uintptr_t address, size_t code_size, void* dest);
void install_hook (const char module_name[], const char import_name[], LPVOID new_handler, LPVOID* old_handler_out);
void install_patch (uintptr_t address, size_t patch_size, const void* patch);
