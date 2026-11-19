#pragma once
/* Called before touching SMHC hardware. No compile-time attestation bypass. */
NTSTATUS A733VerifyFirmwareHandoff(PVOID PrivateExtension);
