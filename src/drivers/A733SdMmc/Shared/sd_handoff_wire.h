#ifndef A733_SD_HANDOFF_WIRE_H
#define A733_SD_HANDOFF_WIRE_H
/* Little-endian, padding-free firmware -> ACPI -> Windows protocol.
 * This is register-readback evidence, NOT a voltage measurement or a signature.
 * No pointers, kernel/UEFI types, or host ABI layouts cross this boundary. */
#define A733_SD_PROOF_SIZE 40U
#define A733_SD_PROOF_FLAGS 0x3FU
#define A733_SD_PROOF_BASE 0x04020000U
#define A733_SD_PROOF_CLOCK 24000000U
typedef char A733WireRequires32BitUnsigned[(sizeof(unsigned int) == 4) ? 1 : -1];
static inline unsigned int A733WireRead32(const unsigned char *p)
{
    return (unsigned int)p[0] | ((unsigned int)p[1] << 8) |
           ((unsigned int)p[2] << 16) | ((unsigned int)p[3] << 24);
}
static inline void A733WireWrite32(unsigned char *p, unsigned int v)
{
    unsigned int i;
    for (i = 0; i < 4; ++i) p[i] = (unsigned char)(v >> (8U * i));
}
static inline void A733WireBuild(unsigned char *p, unsigned int vmmc,
                                  unsigned int vqmmc)
{
    unsigned int i, sum = 0;
    for (i = 0; i < A733_SD_PROOF_SIZE; ++i) p[i] = 0;
    A733WireWrite32(p, 0x48443341U); /* A3DH */
    A733WireWrite32(p + 4, 1U);
    A733WireWrite32(p + 8, A733_SD_PROOF_SIZE);
    /* PMIC read, enabled rails, pin readback, clock readback,
     * final ExitBootServices recheck, committed ACPI namespace. */
    A733WireWrite32(p + 12, A733_SD_PROOF_FLAGS);
    A733WireWrite32(p + 16, A733_SD_PROOF_BASE);
    A733WireWrite32(p + 20, A733_SD_PROOF_CLOCK);
    A733WireWrite32(p + 24, vmmc);
    A733WireWrite32(p + 28, vqmmc);
    for (i = 0; i < 36; i += 4) sum += A733WireRead32(p + i);
    A733WireWrite32(p + 36, 0U - sum);
}
static inline int A733WireValidate(const unsigned char *p, unsigned int size)
{
    unsigned int i, sum = 0, a, b;
    if (!p || size != A733_SD_PROOF_SIZE) return 0;
    if (A733WireRead32(p) != 0x48443341U || A733WireRead32(p + 4) != 1U ||
        A733WireRead32(p + 8) != A733_SD_PROOF_SIZE ||
        A733WireRead32(p + 12) != A733_SD_PROOF_FLAGS ||
        A733WireRead32(p + 16) != A733_SD_PROOF_BASE ||
        A733WireRead32(p + 20) != A733_SD_PROOF_CLOCK ||
        A733WireRead32(p + 32) != 0U) return 0;
    a = A733WireRead32(p + 24); b = A733WireRead32(p + 28);
    if (a < 3000000U || a > 3600000U || b < 3000000U || b > 3600000U) return 0;
    for (i = 0; i < A733_SD_PROOF_SIZE; i += 4) sum += A733WireRead32(p + i);
    return sum == 0U;
}
#endif
