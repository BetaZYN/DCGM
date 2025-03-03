/*
 * Copyright (c) 2024, NVIDIA CORPORATION.  All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include "Software.h"
#include "DcgmError.h"
#include "DcgmGPUHardwareLimits.h"
#include "DcgmLogging.h"
#include "dcgm_errors.h"
#include "dcgm_fields_internal.hpp"
#include <assert.h>
#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fmt/format.h>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string.h>
#include <unistd.h>

#ifdef __x86_64__
__asm__(".symver memcpy,memcpy@GLIBC_2.2.5");
#endif

#define DCGM_STRINGIFY_IMPL(x) #x
#define DCGM_STRINGIFY(x)      DCGM_STRINGIFY_IMPL(x)

#ifndef DCGM_NVML_SONAME
#ifndef DCGM_NVML_SOVERSION
#define DCGM_NVML_SOVERSION 1
#endif
#define DCGM_NVML_SONAME "libnvidia-ml.so." DCGM_STRINGIFY(DCGM_NVML_SOVERSION)
#else
#ifdef DCGM_NVML_SOVERSION
#pragma message DCGM_NVML_SONAME set explicitly; DCGM_NVML_SOVERSION ignored !
#endif
#endif

#ifndef DCGM_CUDA_SONAME
#ifndef DCGM_CUDA_SOVERSION
#define DCGM_CUDA_SOVERSION 1
#endif
#define DCGM_CUDA_SONAME "libcuda.so." DCGM_STRINGIFY(DCGM_CUDA_SOVERSION)
#else
#ifdef DCGM_CUDA_SOVERSION
#pragma message DCGM_CUDA_SONAME set explicitly; DCGM_CUDA_SOVERSION ignored !
#endif
#endif

#ifndef DCGM_CUDART_SONAME
#ifndef DCGM_CUDART_SOVERSION
#define DCGM_CUDART_SOVERSION 1
#endif
#define DCGM_CUDART_SONAME "libcudart.so." DCGM_STRINGIFY(DCGM_CUDART_SOVERSION)
#else
#ifdef DCGM_CUDART_SOVERSION
#pragma message DCGM_CUDART_SONAME set explicitly; DCGM_CUDART_SOVERSION ignored !
#endif
#endif

#ifndef DCGM_CUBLAS_SONAME
#ifndef DCGM_CUBLAS_SOVERSION
#define DCGM_CUBLAS_SOVERSION 1
#endif
#define DCGM_CUBLAS_SONAME "libcublas.so." DCGM_STRINGIFY(DCGM_CUBLAS_SOVERSION)
#else
#ifdef DCGM_CUBLAS_SOVERSION
#pragma message DCGM_CUBLAS_SONAME set explicitly; DCGM_CUBLAS_SOVERSION ignored !
#endif
#endif

Software::Software(dcgmHandle_t handle, dcgmDiagPluginGpuList_t *gpuInfo)
    : m_dcgmRecorder(handle)
    , m_dcgmSystem()
    , m_handle(handle)
    , m_gpuInfo()
{
    m_infoStruct.testIndex        = DCGM_SOFTWARE_INDEX;
    m_infoStruct.shortDescription = "Software deployment checks plugin.";
    m_infoStruct.testGroups       = "Software";
    m_infoStruct.selfParallel     = true;
    m_infoStruct.logFileTag       = SW_PLUGIN_NAME;

    tp = new TestParameters();
    tp->AddString(PS_RUN_IF_GOM_ENABLED, "True");
    tp->AddString(SW_STR_DO_TEST, "None");
    tp->AddString(SW_STR_REQUIRE_PERSISTENCE, "True");
    tp->AddString(SW_STR_SKIP_DEVICE_TEST, "False");
    m_infoStruct.defaultTestParameters = tp;

    if (gpuInfo == nullptr)
    {
        DcgmError d { DcgmError::GpuIdTag::Unknown };
        DCGM_ERROR_FORMAT_MESSAGE(DCGM_FR_INTERNAL, d, "No GPU information specified");
        AddError(SW_PLUGIN_NAME, d);
    }
    else
    {
        m_gpuInfo = *gpuInfo;
        InitializeForGpuList(SW_PLUGIN_NAME, *gpuInfo);
    }

    m_dcgmSystem.Init(handle);
}

void Software::Go(std::string const &testName,
                  unsigned int numParameters,
                  dcgmDiagPluginTestParameter_t const *tpStruct)
{
    if (UsingFakeGpus())
    {
        log_error("Plugin is using fake gpus");
        SetResult(testName, NVVS_RESULT_PASS);
        TestParameters testParameters(*(m_infoStruct.defaultTestParameters));
        testParameters.SetFromStruct(numParameters, tpStruct);
        if (testParameters.GetString(SW_STR_DO_TEST) == "page_retirement")
        {
            checkPageRetirement();
            checkRowRemapping();
        }
        return;
    }

    TestParameters testParameters(*(m_infoStruct.defaultTestParameters));
    testParameters.SetFromStruct(numParameters, tpStruct);

    if (testParameters.GetString(SW_STR_DO_TEST) == "denylist")
        checkDenylist();
    else if (testParameters.GetString(SW_STR_DO_TEST) == "permissions")
    {
        checkPermissions(testParameters.GetBoolFromString(SW_STR_CHECK_FILE_CREATION),
                         testParameters.GetBoolFromString(SW_STR_SKIP_DEVICE_TEST));
    }
    else if (testParameters.GetString(SW_STR_DO_TEST) == "libraries_nvml")
        checkLibraries(CHECK_NVML);
    else if (testParameters.GetString(SW_STR_DO_TEST) == "libraries_cuda")
        checkLibraries(CHECK_CUDA);
    else if (testParameters.GetString(SW_STR_DO_TEST) == "libraries_cudatk")
        checkLibraries(CHECK_CUDATK);
    else if (testParameters.GetString(SW_STR_DO_TEST) == "persistence_mode")
    {
        int shouldCheckPersistence = testParameters.GetBoolFromString(SW_STR_REQUIRE_PERSISTENCE);

        if (!shouldCheckPersistence)
        {
            log_info("Skipping persistence check");
            SetResult(testName, NVVS_RESULT_SKIP);
        }
        else
        {
            checkPersistenceMode();
        }
    }
    else if (testParameters.GetString(SW_STR_DO_TEST) == "env_variables")
        checkForBadEnvVaribles();
    else if (testParameters.GetString(SW_STR_DO_TEST) == "graphics_processes")
        checkForGraphicsProcesses();
    else if (testParameters.GetString(SW_STR_DO_TEST) == "page_retirement")
    {
        checkPageRetirement();
        checkRowRemapping();
    }
    else if (testParameters.GetString(SW_STR_DO_TEST) == "inforom")
        checkInforom();
}

bool Software::CountDevEntry(std::string const &entryName)
{
    if (entryName.compare(0, 6, "nvidia") == 0)
    {
        for (size_t i = 6; i < entryName.size(); i++)
        {
            if (!isdigit(entryName.at(i)))
            {
                return false;
            }
        }
        return true;
    }

    return false;
}

bool Software::checkPermissions(bool checkFileCreation, bool skipDeviceTest)
{
    // check how many devices we see reporting and compare to
    // the number of devices listed in /dev
    unsigned int gpuCount    = 0;
    unsigned int deviceCount = 0;

    DIR *dir;
    struct dirent *ent;
    std::string dirName = "/dev";

    // Count the number of GPUs
    std::vector<unsigned int> gpuIds;
    dcgmReturn_t ret = m_dcgmSystem.GetAllDevices(gpuIds);
    if (DCGM_ST_OK != ret)
    {
        return false;
    }
    gpuCount = gpuIds.size();

    // everything below here is not necessarily a failure
    SetResult(SW_PLUGIN_NAME, NVVS_RESULT_PASS);
    if (skipDeviceTest == false)
    {
        dir = opendir(dirName.c_str());

        if (NULL == dir)
            return false;

        ent = readdir(dir);

        std::vector<DcgmError> accessWarnings;

        while (NULL != ent)
        {
            std::string entryName = ent->d_name;
            if (CountDevEntry(entryName))
            {
                std::stringstream ss;
                ss << dirName << "/" << entryName;
                if (access(ss.str().c_str(), R_OK) != 0)
                {
                    DcgmError d { DcgmError::GpuIdTag::Unknown };
                    DCGM_ERROR_FORMAT_MESSAGE(DCGM_FR_NO_ACCESS_TO_FILE, d, ss.str().c_str(), strerror(errno));
                    accessWarnings.emplace_back(d);
                }
                else
                {
                    deviceCount++;
                }
            }

            ent = readdir(dir);
        }
        closedir(dir);

        if (deviceCount < gpuCount)
        {
            DcgmError d { DcgmError::GpuIdTag::Unknown };
            DCGM_ERROR_FORMAT_MESSAGE(DCGM_FR_DEVICE_COUNT_MISMATCH, d);
            AddError(SW_PLUGIN_NAME, d);
            for (auto &warning : accessWarnings)
            {
                AddError(SW_PLUGIN_NAME, warning);
            }
            SetResult(SW_PLUGIN_NAME, NVVS_RESULT_WARN);
        }
    }

    if (checkFileCreation)
    {
        // Make sure we have the ability to create files in this directory
        if (euidaccess(".", W_OK))
        {
            char cwd[1024];
            char const *working_dir = getcwd(cwd, sizeof(cwd));

            DcgmError d { DcgmError::GpuIdTag::Unknown };
            d.SetCode(DCGM_FR_FILE_CREATE_PERMISSIONS);
            d.SetMessage(fmt::format("No permission to create a file in directory '{}'", working_dir));
            d.SetNextSteps(DCGM_FR_FILE_CREATE_PERMISSIONS_NEXT);
            AddError(SW_PLUGIN_NAME, d);
            return false;
        }
    }

    return false;
}

// This function suggests time-of-check/time-of-use (TOCTOU) race conditions
bool Software::checkLibraries(libraryCheck_t checkLib)
{
    // Check whether the NVML, CUDA, or CUDA toolkit libraries can be found with sufficient permissions
    using span_t   = std::span<char const *const>;
    using result_t = nvvsPluginResult_t;

    auto const check = [this](span_t const libraries, span_t const diagnostics, result_t const failureCode) {
        bool failure = false;
        std::string error;

        for (char const *const library : libraries)
        {
            if (!findLib(library, error))
            {
                DcgmError d { DcgmError::GpuIdTag::Unknown };
                DCGM_ERROR_FORMAT_MESSAGE(DCGM_FR_CANNOT_OPEN_LIB, d, library, error.c_str());
                AddError(SW_PLUGIN_NAME, d);
                SetResult(SW_PLUGIN_NAME, failureCode);
                failure = true;
            }
        }

        if (failure)
        {
            for (char const *const diagnostic : diagnostics)
            {
                AddInfo(SW_PLUGIN_NAME, diagnostic);
            }
        }

        return failure;
    };

    switch (checkLib)
    {
        case CHECK_NVML:
        {
            static constexpr char const *libraries[]   = { DCGM_NVML_SONAME };
            static constexpr char const *diagnostics[] = {
                "The NVML main library could not be found in the default search paths.",
                "Please check to see if it is installed or that LD_LIBRARY_PATH contains the path to " DCGM_NVML_SONAME,
                "Skipping remainder of tests."
            };

            return check(libraries, diagnostics, NVVS_RESULT_FAIL);
        }
        case CHECK_CUDA:
        {
            static constexpr char const *libraries[]   = { DCGM_CUDA_SONAME };
            static constexpr char const *diagnostics[] = { "The CUDA main library could not be found."
                                                           "Skipping remainder of tests." };

            return check(libraries, diagnostics, NVVS_RESULT_WARN);
        }
        case CHECK_CUDATK:
        {
            static constexpr char const *libraries[] = { DCGM_CUDART_SONAME, DCGM_CUBLAS_SONAME };
            static constexpr char const *diagnostics[]
                = { "The CUDA Toolkit libraries could not be found.",
                    "Is LD_LIBRARY_PATH set to the 64-bit library path? (usually /usr/local/cuda/lib64)",
                    "Some tests will not run." };

            return check(libraries, diagnostics, NVVS_RESULT_WARN);
        }
        default:
        {
            // should never get here
            DcgmError d { DcgmError::GpuIdTag::Unknown };
            DCGM_ERROR_FORMAT_MESSAGE(DCGM_FR_BAD_PARAMETER, d, __func__);
            AddError(SW_PLUGIN_NAME, d);
            SetResult(SW_PLUGIN_NAME, NVVS_RESULT_FAIL);
            return true;
        }
    }
}

bool Software::checkDenylist()
{
    // check whether the nouveau driver is installed and if so, fail this test
    bool status = false;

    std::string const searchPaths[] = { "/sys/bus/pci/devices", "/sys/bus/pci_express/devices" };
    std::string const driverDirs[]  = { "driver", "subsystem/drivers" };

    std::vector<std::string> const denyList = { "nouveau" };

    for (int i = 0; i < sizeof(searchPaths) / sizeof(searchPaths[0]); i++)
    {
        DIR *dir;
        struct dirent *ent;

        dir = opendir(searchPaths[i].c_str());

        if (NULL == dir)
            continue;

        ent = readdir(dir);
        while (NULL != ent)
        {
            if ((strcmp(ent->d_name, ".") == 0) || (strcmp(ent->d_name, "..") == 0))
            {
                ent = readdir(dir);
                continue;
            }
            for (int j = 0; j < sizeof(driverDirs) / sizeof(driverDirs[0]); j++)
            {
                std::string baseDir = searchPaths[i];
                std::stringstream testPath;
                testPath << baseDir << "/" << ent->d_name << "/" << driverDirs[j];
                if (checkDriverPathDenylist(testPath.str(), denyList))
                {
                    SetResult(SW_PLUGIN_NAME, NVVS_RESULT_FAIL);
                    status = true;
                }
            }
            ent = readdir(dir);
        }
        closedir(dir);
    }
    if (!status)
        SetResult(SW_PLUGIN_NAME, NVVS_RESULT_PASS);
    return status;
}

int Software::checkDriverPathDenylist(std::string driverPath, std::vector<std::string> const &denyList)
{
    int ret;
    char symlinkTarget[1024];
    ret = readlink(driverPath.c_str(), symlinkTarget, sizeof(symlinkTarget));
    if (ret >= (signed int)sizeof(symlinkTarget))
    {
        assert(0);
        return ENAMETOOLONG;
    }
    else if (ret < 0)
    {
        int errorno = errno;

        switch (errorno)
        {
            case ENOENT:
                // driverPath does not exist, ignore it
                // this driver doesn't use this path format
                return 0;
            case EINVAL: // not a symlink
                return 0;

            case EACCES:
            case ENOTDIR:
            case ELOOP:
            case ENAMETOOLONG:
            case EIO:
            default:
                // Something bad happened
                return errorno;
        }
    }
    else
    {
        symlinkTarget[ret] = '\0'; // readlink doesn't null terminate
        for (auto const &item : denyList)
        {
            if (strcmp(item.c_str(), basename(symlinkTarget)) == 0)
            {
                DcgmError d { DcgmError::GpuIdTag::Unknown };
                DCGM_ERROR_FORMAT_MESSAGE(DCGM_FR_DENYLISTED_DRIVER, d, item.c_str());
                AddError(SW_PLUGIN_NAME, d);
                return 1;
            }
        }
    }

    return 0;
}

bool Software::findLib(std::string library, std::string &error)
{
    // On Linux, the search procedure considers
    // 1. (ELF binaries) the directories described by the binary RPATH (if the RUNPATH tag is absent)
    // 2. the directories described by the LD_LIBRARY_PATH environment variable
    // 3. (ELF binaries) the directories described by the binary RUNPATH (if the RUNPATH tag is present)
    // 4. the /etc/ld.so.cache
    // 5. the /lib directory
    // 6. the /usr/lib directory
    void *const handle = dlopen(library.c_str(), RTLD_NOW);
    if (!handle)
    {
        error = dlerror();
        return false;
    }
    dlclose(handle);
    return true;
}

int Software::checkForGraphicsProcesses()
{
    std::vector<unsigned int>::const_iterator gpuIt;
    unsigned int gpuId;
    unsigned int flags = DCGM_FV_FLAG_LIVE_DATA;
    dcgmFieldValue_v2 graphicsPidsVal;

    for (gpuIt = m_gpuList.begin(); gpuIt != m_gpuList.end(); gpuIt++)
    {
        gpuId = *gpuIt;

        dcgmReturn_t ret
            = m_dcgmRecorder.GetCurrentFieldValue(gpuId, DCGM_FI_DEV_GRAPHICS_PIDS, graphicsPidsVal, flags);

        if (ret != DCGM_ST_OK)
        {
            DcgmError d { gpuId };
            DCGM_ERROR_FORMAT_MESSAGE_DCGM(DCGM_FR_FIELD_QUERY, d, ret, "graphics_pids", gpuId);
            AddErrorForGpu(SW_PLUGIN_NAME, gpuId, d);
            SetResult(SW_PLUGIN_NAME, NVVS_RESULT_FAIL);
            continue;
        }

        if (graphicsPidsVal.status != DCGM_ST_OK)
        {
            std::stringstream buf;
            buf << "Error getting the graphics pids for GPU " << gpuId << ". Status = " << graphicsPidsVal.status
                << " skipping check.";
            std::string info(buf.str());
            DCGM_LOG_WARNING << info;
            AddInfo(SW_PLUGIN_NAME, info);
            continue;
        }
        else if (graphicsPidsVal.value.blob[0] != '\0')
        {
            // If there's any information here, it means a process is running
            DcgmError d { gpuId };
            DCGM_ERROR_FORMAT_MESSAGE(DCGM_FR_GRAPHICS_PROCESSES, d);
            AddErrorForGpu(SW_PLUGIN_NAME, gpuId, d);
            SetResult(SW_PLUGIN_NAME, NVVS_RESULT_WARN);
        }
    }

    return 0;
}

int Software::checkPersistenceMode()
{
    unsigned int gpuId;
    std::vector<unsigned int>::const_iterator gpuIt;

    for (gpuIt = m_gpuList.begin(); gpuIt != m_gpuList.end(); gpuIt++)
    {
        gpuId = *gpuIt;

        for (unsigned int sindex = 0; sindex < m_gpuInfo.numGpus; sindex++)
        {
            if (m_gpuInfo.gpus[sindex].gpuId != gpuId)
            {
                continue;
            }

            if (m_gpuInfo.gpus[sindex].attributes.settings.persistenceModeEnabled == false)
            {
                DcgmError d { gpuId };
                DCGM_ERROR_FORMAT_MESSAGE(DCGM_FR_PERSISTENCE_MODE, d, gpuId);
                AddWarning(SW_PLUGIN_NAME, d.GetMessage());
                SetResult(SW_PLUGIN_NAME, NVVS_RESULT_WARN);
                break;
            }
        }
    }

    return 0;
}

int Software::checkPageRetirement()
{
    unsigned int gpuId;
    std::vector<unsigned int>::const_iterator gpuIt;
    dcgmFieldValue_v2 pendingRetirementsFieldValue;
    dcgmFieldValue_v2 dbeFieldValue;
    dcgmFieldValue_v2 sbeFieldValue;
    dcgmReturn_t ret;
    int64_t retiredPagesTotal;

    /* Flags to pass to dcgmRecorder.GetCurrentFieldValue. Get live data since we're not watching the fields ahead of
     * time */
    unsigned int flags = DCGM_FV_FLAG_LIVE_DATA;

    if (UsingFakeGpus())
    {
        /* fake gpus don't support live data */
        flags = 0;
    }

    for (gpuIt = m_gpuList.begin(); gpuIt != m_gpuList.end(); gpuIt++)
    {
        gpuId = *gpuIt;
        // Check for pending page retirements
        ret = m_dcgmRecorder.GetCurrentFieldValue(
            gpuId, DCGM_FI_DEV_RETIRED_PENDING, pendingRetirementsFieldValue, flags);
        if (ret != DCGM_ST_OK)
        {
            DcgmError d { gpuId };
            DCGM_ERROR_FORMAT_MESSAGE_DCGM(DCGM_FR_FIELD_QUERY, d, ret, "retired_pages_pending", gpuId);
            AddErrorForGpu(SW_PLUGIN_NAME, gpuId, d);
            SetResult(SW_PLUGIN_NAME, NVVS_RESULT_FAIL);
            continue;
        }

        if (pendingRetirementsFieldValue.status != DCGM_ST_OK
            || DCGM_INT64_IS_BLANK(pendingRetirementsFieldValue.value.i64))
        {
            DCGM_LOG_WARNING << "gpuId " << gpuId << " returned status " << pendingRetirementsFieldValue.status
                             << ", value " << pendingRetirementsFieldValue.value.i64
                             << "for DCGM_FI_DEV_RETIRED_PENDING. Skipping this check.";
        }
        else if (pendingRetirementsFieldValue.value.i64 > 0)
        {
            dcgmFieldValue_v2 volDbeVal = {};
            ret = m_dcgmRecorder.GetCurrentFieldValue(gpuId, DCGM_FI_DEV_ECC_DBE_VOL_TOTAL, volDbeVal, flags);
            if (ret == DCGM_ST_OK && (volDbeVal.value.i64 > 0 && !DCGM_INT64_IS_BLANK(volDbeVal.value.i64)))
            {
                DcgmError d { gpuId };
                DCGM_ERROR_FORMAT_MESSAGE(DCGM_FR_DBE_PENDING_PAGE_RETIREMENTS, d, gpuId);
                AddErrorForGpu(SW_PLUGIN_NAME, gpuId, d);
                SetResult(SW_PLUGIN_NAME, NVVS_RESULT_FAIL);
            }
            else
            {
                DcgmError d { gpuId };
                DCGM_ERROR_FORMAT_MESSAGE(DCGM_FR_PENDING_PAGE_RETIREMENTS, d, gpuId);
                AddErrorForGpu(SW_PLUGIN_NAME, gpuId, d);
                SetResult(SW_PLUGIN_NAME, NVVS_RESULT_FAIL);
            }
            /*
             * Halt nvvs for failures related to 'pending page retirements' or 'RETIRED_DBE/SBE'. Please be aware that
             * we will not stop for internal DCGM failures, such as issues with retrieving the current field value.
             */
            main_should_stop.store(1);
            continue;
        }

        // Check total page retirement count
        retiredPagesTotal = 0;

        // DBE retired pages
        ret = m_dcgmRecorder.GetCurrentFieldValue(gpuId, DCGM_FI_DEV_RETIRED_DBE, dbeFieldValue, flags);
        if (ret != DCGM_ST_OK)
        {
            DcgmError d { gpuId };
            DCGM_ERROR_FORMAT_MESSAGE_DCGM(DCGM_FR_FIELD_QUERY, d, ret, "retired_pages_dbe", gpuId);
            AddErrorForGpu(SW_PLUGIN_NAME, gpuId, d);
            SetResult(SW_PLUGIN_NAME, NVVS_RESULT_FAIL);
            continue;
        }

        if (dbeFieldValue.status != DCGM_ST_OK || DCGM_INT64_IS_BLANK(dbeFieldValue.value.i64))
        {
            DCGM_LOG_WARNING << "gpuId " << gpuId << " returned status " << dbeFieldValue.status << ", value "
                             << dbeFieldValue.value.i64 << "for DCGM_FI_DEV_RETIRED_DBE. Skipping this check.";
        }
        else
        {
            retiredPagesTotal += dbeFieldValue.value.i64;
        }

        // SBE retired pages
        ret = m_dcgmRecorder.GetCurrentFieldValue(gpuId, DCGM_FI_DEV_RETIRED_SBE, sbeFieldValue, flags);
        if (ret != DCGM_ST_OK)
        {
            DcgmError d { gpuId };
            DCGM_ERROR_FORMAT_MESSAGE_DCGM(DCGM_FR_FIELD_QUERY, d, ret, "retired_pages_sbe", gpuId);
            AddErrorForGpu(SW_PLUGIN_NAME, gpuId, d);
            SetResult(SW_PLUGIN_NAME, NVVS_RESULT_FAIL);
            continue;
        }

        if (sbeFieldValue.status != DCGM_ST_OK || DCGM_INT64_IS_BLANK(sbeFieldValue.value.i64))
        {
            DCGM_LOG_WARNING << "gpuId " << gpuId << " returned status " << sbeFieldValue.status << ", value "
                             << sbeFieldValue.value.i64 << "for DCGM_FI_DEV_RETIRED_SBE. Skipping this check.";
        }
        else
        {
            retiredPagesTotal += sbeFieldValue.value.i64;
        }

        if (retiredPagesTotal >= DCGM_LIMIT_MAX_RETIRED_PAGES)
        {
            DcgmError d { gpuId };
            DCGM_ERROR_FORMAT_MESSAGE(DCGM_FR_RETIRED_PAGES_LIMIT, d, DCGM_LIMIT_MAX_RETIRED_PAGES, gpuId);
            AddErrorForGpu(SW_PLUGIN_NAME, gpuId, d);
            SetResult(SW_PLUGIN_NAME, NVVS_RESULT_FAIL);
            main_should_stop.store(1);
            continue;
        }
    }

    return 0;
}

int Software::checkRowRemapping()
{
    unsigned int gpuId;
    std::vector<unsigned int>::const_iterator gpuIt;
    dcgmFieldValue_v2 pendingRowRemap;
    dcgmFieldValue_v2 rowRemapFailure;
    dcgmReturn_t ret;

    /* Flags to pass to dcgmRecorder.GetCurrentFieldValue. Get live data since we're not watching the fields ahead of
     * time */
    unsigned int flags = DCGM_FV_FLAG_LIVE_DATA;

    if (UsingFakeGpus())
    {
        /* fake gpus don't support live data */
        flags = 0;
    }

    for (gpuIt = m_gpuList.begin(); gpuIt != m_gpuList.end(); gpuIt++)
    {
        gpuId = *gpuIt;

        memset(&rowRemapFailure, 0, sizeof(rowRemapFailure));

        // Row remap failure
        ret = m_dcgmRecorder.GetCurrentFieldValue(gpuId, DCGM_FI_DEV_ROW_REMAP_FAILURE, rowRemapFailure, flags);
        if (ret != DCGM_ST_OK)
        {
            DcgmError d { gpuId };
            DCGM_ERROR_FORMAT_MESSAGE_DCGM(DCGM_FR_FIELD_QUERY, d, ret, "row_remap_failure", gpuId);
            AddErrorForGpu(SW_PLUGIN_NAME, gpuId, d);
            SetResultForGpu(SW_PLUGIN_NAME, gpuId, NVVS_RESULT_FAIL);
            continue;
        }

        if (rowRemapFailure.status != DCGM_ST_OK || DCGM_INT64_IS_BLANK(rowRemapFailure.value.i64))
        {
            DCGM_LOG_INFO << "gpuId " << gpuId << " returned status " << rowRemapFailure.status << ", value "
                          << rowRemapFailure.value.i64 << "for DCGM_FI_DEV_ROW_REMAP_FAILURE. Skipping this check.";
        }
        else if (rowRemapFailure.value.i64 > 0)
        {
            DcgmError d { gpuId };
            DCGM_ERROR_FORMAT_MESSAGE(DCGM_FR_ROW_REMAP_FAILURE, d, gpuId);
            AddErrorForGpu(SW_PLUGIN_NAME, gpuId, d);
            SetResultForGpu(SW_PLUGIN_NAME, gpuId, NVVS_RESULT_FAIL);

            /*
             * Halt nvvs for failures related to 'row remap/pending' or 'uncorrectable remapped row'. Please be aware
             * that we will not stop for internal DCGM failures, such as issues with retrieving the current field value.
             */
            main_should_stop.store(1);
            continue;
        }

        memset(&pendingRowRemap, 0, sizeof(pendingRowRemap));

        // Check for pending row remappings
        ret = m_dcgmRecorder.GetCurrentFieldValue(gpuId, DCGM_FI_DEV_ROW_REMAP_PENDING, pendingRowRemap, flags);
        if (ret != DCGM_ST_OK)
        {
            DcgmError d { gpuId };
            DCGM_ERROR_FORMAT_MESSAGE_DCGM(DCGM_FR_FIELD_QUERY, d, ret, "row_remap_pending", gpuId);
            AddErrorForGpu(SW_PLUGIN_NAME, gpuId, d);
            SetResultForGpu(SW_PLUGIN_NAME, gpuId, NVVS_RESULT_FAIL);
            continue;
        }

        if (pendingRowRemap.status != DCGM_ST_OK || DCGM_INT64_IS_BLANK(pendingRowRemap.value.i64))
        {
            DCGM_LOG_INFO << "gpuId " << gpuId << " returned status " << pendingRowRemap.status << ", value "
                          << pendingRowRemap.value.i64 << "for DCGM_FI_DEV_ROW_REMAP_PENDING. Skipping this check.";
        }
        else if (pendingRowRemap.value.i64 > 0)
        {
            dcgmFieldValue_v2 uncRemap = {};
            ret = m_dcgmRecorder.GetCurrentFieldValue(gpuId, DCGM_FI_DEV_UNCORRECTABLE_REMAPPED_ROWS, uncRemap, flags);
            if (ret == DCGM_ST_OK && (uncRemap.value.i64 > 0 && !DCGM_INT64_IS_BLANK(uncRemap.value.i64)))
            {
                DcgmError d { gpuId };
                DCGM_ERROR_FORMAT_MESSAGE(DCGM_FR_UNCORRECTABLE_ROW_REMAP, d, gpuId);
                AddErrorForGpu(SW_PLUGIN_NAME, gpuId, d);
                SetResultForGpu(SW_PLUGIN_NAME, gpuId, NVVS_RESULT_FAIL);
            }
            else
            {
                DcgmError d { gpuId };
                DCGM_ERROR_FORMAT_MESSAGE(DCGM_FR_PENDING_ROW_REMAP, d, gpuId);
                AddErrorForGpu(SW_PLUGIN_NAME, gpuId, d);
                SetResultForGpu(SW_PLUGIN_NAME, gpuId, NVVS_RESULT_FAIL);
            }
            main_should_stop.store(1);
        }
    }

    return 0;
}

int Software::checkInforom()
{
    std::vector<unsigned int>::const_iterator gpuIt;
    unsigned int flags = DCGM_FV_FLAG_LIVE_DATA;
    dcgmFieldValue_v2 inforomValidVal;

    for (gpuIt = m_gpuList.begin(); gpuIt != m_gpuList.end(); gpuIt++)
    {
        unsigned int gpuId = *gpuIt;

        dcgmReturn_t ret
            = m_dcgmRecorder.GetCurrentFieldValue(gpuId, DCGM_FI_DEV_INFOROM_CONFIG_VALID, inforomValidVal, flags);

        if (ret != DCGM_ST_OK)
        {
            DcgmError d { gpuId };
            DCGM_ERROR_FORMAT_MESSAGE_DCGM(DCGM_FR_FIELD_QUERY, d, ret, "inforom_config_valid", gpuId);
            AddErrorForGpu(SW_PLUGIN_NAME, gpuId, d);
            SetResult(SW_PLUGIN_NAME, NVVS_RESULT_FAIL);
            continue;
        }

        if ((inforomValidVal.status == DCGM_ST_NOT_SUPPORTED)
            || (inforomValidVal.status == DCGM_ST_OK && DCGM_INT64_IS_BLANK(inforomValidVal.value.i64)))
        {
            std::stringstream buf;
            buf << "DCGM returned status " << inforomValidVal.status << " for GPU " << gpuId
                << " when checking the validity of the inforom. Skipping this check.";
            std::string info(buf.str());
            DCGM_LOG_WARNING << info;
            AddInfo(SW_PLUGIN_NAME, info);
            SetResult(SW_PLUGIN_NAME, NVVS_RESULT_SKIP);
            continue;
        }
        else if (inforomValidVal.status != DCGM_ST_OK)
        {
            std::stringstream buf;
            buf << "DCGM returned status " << inforomValidVal.status << " for GPU " << gpuId
                << " when checking the validity of the inforom. Skipping this check.";
            std::string info(buf.str());
            DCGM_LOG_WARNING << info;
            AddInfo(SW_PLUGIN_NAME, info);
            continue;
        }
        else if (inforomValidVal.value.i64 == 0)
        {
            // Inforom is not valid
            DcgmError d { gpuId };
            DCGM_ERROR_FORMAT_MESSAGE(DCGM_FR_CORRUPT_INFOROM, d, gpuId);
            AddErrorForGpu(SW_PLUGIN_NAME, gpuId, d);
            SetResult(SW_PLUGIN_NAME, NVVS_RESULT_FAIL);
            continue;
        }
    }

    return 0;
}

int Software::checkForBadEnvVaribles()
{
    std::vector<std::string> checkKeys;
    std::vector<std::string>::iterator checkKeysIt;
    std::string checkKey;

    /* Env variables to look for */
    checkKeys.push_back(std::string("NSIGHT_CUDA_DEBUGGER"));
    checkKeys.push_back(std::string("CUDA_INJECTION32_PATH"));
    checkKeys.push_back(std::string("CUDA_INJECTION64_PATH"));
    checkKeys.push_back(std::string("CUDA_AUTO_BOOST"));
    checkKeys.push_back(std::string("CUDA_ENABLE_COREDUMP_ON_EXCEPTION"));
    checkKeys.push_back(std::string("CUDA_COREDUMP_FILE"));
    checkKeys.push_back(std::string("CUDA_DEVICE_WAITS_ON_EXCEPTION"));
    checkKeys.push_back(std::string("CUDA_PROFILE"));
    checkKeys.push_back(std::string("COMPUTE_PROFILE"));
    checkKeys.push_back(std::string("OPENCL_PROFILE"));

    for (checkKeysIt = checkKeys.begin(); checkKeysIt != checkKeys.end(); checkKeysIt++)
    {
        checkKey = *checkKeysIt;

        /* Does the variable exist in the environment? */
        if (getenv(checkKey.c_str()) == nullptr)
        {
            log_debug("Env Variable {} not found (GOOD)", checkKey.c_str());
            continue;
        }

        /* Variable found. Warn */
        DcgmError d { DcgmError::GpuIdTag::Unknown };
        DCGM_ERROR_FORMAT_MESSAGE(DCGM_FR_BAD_CUDA_ENV, d, checkKey.c_str());
        AddError(SW_PLUGIN_NAME, d);
        SetResult(SW_PLUGIN_NAME, NVVS_RESULT_WARN);
    }

    return 0;
}
