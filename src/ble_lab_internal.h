#pragma once
#include "ble_lab.h"
#include "identity.h"

// Used only by the in-process production CoC runner. The Python service owns
// the lab for its lifetime and excludes pairing while a CoC session runs.
#ifdef __cplusplus
extern "C" {
#endif
PltrIdentityStore *pltr_ble_lab_identity_store(PltrBleLab *lab);
#ifdef __cplusplus
}
#endif
