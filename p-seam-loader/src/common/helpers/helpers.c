// Intel Proprietary 
// 
// Copyright 2021 Intel Corporation All Rights Reserved.
// 
// Your use of this software is governed by the TDX Source Code LIMITED USE LICENSE.
// 
// The Materials are provided “as is,” without any express or implied warranty of any kind including warranties
// of merchantability, non-infringement, title, or fitness for a particular purpose.
/**
 * @file helpers.c
 * @brief Common PSEAMLDR API flow helper functions
 */

#include "helpers.h"

#include "../../../include/pseamldr_api_defs.h"
#include "x86_defs/vmcs_defs.h"
#include "x86_defs/msr_defs.h"

#include "accessors/ia32_accessors.h"
#include "accessors/vt_accessors.h"
#include "memory_handlers/keyhole_manager.h"
#include "seam_sigstruct.h"
#include "crypto/sha384.h"


#ifdef NO_OPT_BUILD
void* memset(void *str, int c, uint32_t n)
{
    basic_memset((uint64_t)str, (uint8_t)c, n);

    return str;
}
#endif

uint64_t get_num_addressable_lps_on_socket(void)
{
    uint32_t eax, ebx, ecx, edx;
    cpu_cache_params_t cpu_cache_params;

    ia32_safe_cpuid(CPUID_DET_CACHE_PARAMS_LEAF, CPUID_DET_CACHE_PARAMS_SUBLEAF, &eax, &ebx, &ecx, &edx);

    cpu_cache_params.raw = eax;
    uint64_t num_addr_lps = cpu_cache_params.max_num_of_lps_sharing_cache + 1;

    // If not a power of 2
    if (num_addr_lps & (num_addr_lps - 1))
    {
        // Round up to the nearest power of 2:
        num_addr_lps = BIT(bit_scan_reverse64(num_addr_lps) + 1);
    }

    return num_addr_lps;
}

uint32_t get_current_lpid(void)
{
    uint32_t eax, ebx, ecx, edx;

    ia32_safe_cpuid(CPUID_GET_TOPOLOGY_LEAF, 0, &eax, &ebx, &ecx, &edx);

    return edx;
}

void basic_memset(uint64_t dst, uint64_t dst_bytes, uint8_t val, uint64_t nbytes)
{
    pseamldr_sanity_check (dst_bytes >= nbytes, SCEC_HELPERS_SOURCE, 2);

   _ASM_VOLATILE_ ("rep; stosb;"
                    :
                    :"c"(nbytes), "a"(val), "D"(dst)
                    :"memory");
}

void basic_memset_to_zero(void * dst, uint64_t nbytes)
{
    basic_memset((uint64_t)dst, nbytes, 0, nbytes);
}

static bool_t shared_hpa_check(pa_t hpa)
{
    pseamldr_data_t* pseamldr_data = get_pseamldr_data();

    // 1) Check that no bits above MAX_PA are set

    if (!is_pa_smaller_than_max_pa(hpa.raw))
    {
        return false;
    }

    // 2) Check that the provided HPA is outside SEAMRR.
    // This check is correct because it is called from shared_hpa_check_with_pwr_2_alignment,
    // after we know that the HPA is aligned on the size of the of structure that it points on.
    // Which means that we can check only if the address itself is in the range of SEAMRR,
    // and don't need to check the whole structure range.

    if (is_addr_in_range(get_addr_from_pa(hpa), pseamldr_data->system_info.seamrr_base,
                                                pseamldr_data->system_info.seamrr_size))
    {
        return false;
    }

    // 3) Check that HKID bits in the HPA are in the range configured for shared HKIDs (0 to MAX_MKTME_HKIDS – 1).

    if ((uint64_t)get_hkid_from_pa(hpa) >= pseamldr_data->system_info.max_mktme_hkids)
    {
        return false;
    }

    return true;
}

bool_t shared_hpa_check_with_pwr_2_alignment(pa_t hpa, uint64_t size)
{
    if (!is_addr_aligned_pwr_of_2(hpa.raw, size))
    {
        return false;
    }

    return shared_hpa_check(hpa);
}

api_error_type check_and_map_aligned_shared_hpa(pa_t hpa, uint64_t alignment, mapping_type_t mapping_type, void** la)
{
    bool_t errc;

    errc = shared_hpa_check_with_pwr_2_alignment(hpa, alignment);

    if (errc != true)
    {
        return PSEAMLDR_EBADPARAM;
    }

    *la = map_pa((void*)hpa.full_pa, mapping_type);

    return PSEAMLDR_SUCCESS;
}

bool_t is_addr_in_range(uint64_t addr, uint64_t base, uint64_t size)
{
    return ((addr >= base) && (addr < (base + size)));
}

void seamextend_read(seamextend_t* seamextend_dst)
{
    uint64_t buffer_pa = translate_module_va_to_pa((uint64_t)seamextend_dst);
    ia32_wrmsr(IA32_SEAMEXTEND_MSR_ADDR, buffer_pa | 0x1);
}

void seamextend_write(seamextend_t* seamextend_src)
{
    uint64_t buffer_pa = translate_module_va_to_pa((uint64_t)seamextend_src);
    ia32_wrmsr(IA32_SEAMEXTEND_MSR_ADDR, buffer_pa);
}

bool_t safe_memcmp(const void * a, const void * b, uint64_t nbytes)
{
    volatile uint8_t result = 0;

    volatile uint8_t* a_8 = (uint8_t*)a;
    volatile uint8_t* b_8 = (uint8_t*)b;

    for (uint64_t i = 0; i < nbytes; i++)
    {
        result |= (a_8[i] ^ b_8[i]);
    }

    return (result == 0);
}

bool_t compute_and_verify_hash(const uint8_t* message, uint32_t message_len, const uint8_t* expected_hash)
{
    uint64_t calculated_hash[SIZE_OF_SHA384_HASH_IN_QWORDS];

    if (sha384_generate_hash(message, message_len, calculated_hash) != 0)
    {
        FATAL_ERROR();
    }

    return safe_memcmp(calculated_hash, expected_hash, SIZE_OF_SHA384_HASH_IN_BYTES);
}

void ia32_safe_cpuid(uint32_t leaf, uint32_t subleaf, uint32_t *eax, uint32_t *ebx, uint32_t *ecx, uint32_t *edx)
{    
    uint64_t misc_enable = 0;
   
    if (leaf > 2)
    {
        misc_enable = ia32_rdmsr(IA32_MISC_ENABLE_MSR_ADDR);
        if ((misc_enable & IA32_MISC_ENABLE_BOOT_NT4_BIT) != 0) 
        {
            ia32_wrmsr(IA32_MISC_ENABLE_MSR_ADDR, (misc_enable & ~IA32_MISC_ENABLE_BOOT_NT4_BIT));
        }
    }    

    ia32_cpuid(leaf, subleaf, eax, ebx, ecx, edx);
    
    if ((leaf > 2) && ((misc_enable & IA32_MISC_ENABLE_BOOT_NT4_BIT) != 0))
    {
        ia32_wrmsr(IA32_MISC_ENABLE_MSR_ADDR, misc_enable);
    }          
}

