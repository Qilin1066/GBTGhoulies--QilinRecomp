## Crypto (Xc) (5)
- 335 XcSHAInit
- 336 XcSHAUpdate
- 337 XcSHAFinal
- 340 XcHMAC
- 344 XcVerifyPKCS1Signature

## Data exports / keys / hardware info (6)
- 322 XboxHardwareInfo
- 323 XboxHDKey
- 324 XboxKrnlVersion
- 325 XboxSignatureKey
- 354 XboxAlternateSignatureKeys
- 357 IdexChannelObject

## Debug (1)
- 8 DbgPrint

## Executive / pools / settings (6)
- 15 ExAllocatePoolWithTag
- 16 ExEventObjectType
- 17 ExFreePool
- 23 ExQueryPoolBlockSize
- 24 ExQueryNonVolatileSetting
- 164 LaunchDataPage

## File-system cache (1)
- 37 FscSetCacheSize

## HAL / hardware (7)
- 40 HalDiskCachePartitionCount
- 44 HalGetInterruptVector
- 46 HalReadWritePCISpace
- 47 HalRegisterShutdownNotification
- 49 HalReturnToFirmware
- 356 HalBootSMCVideoMode
- 360 HalInitiateShutdown

## I/O manager (4)
- 67 IoCreateSymbolicLink
- 69 IoDeleteSymbolicLink
- 71 IoFileObjectType
- 87 IofCompleteRequest

## Image / section loading (Xe) (4)
- 326 XeImageFileName
- 327 XeLoadSection
- 328 XeUnloadSection
- 355 XePublicKeyData

## Kernel scheduler / sync / time (23)
- 95 KeBugCheck
- 97 KeCancelTimer
- 98 KeConnectInterrupt
- 99 KeDelayExecutionThread
- 100 KeDisconnectInterrupt
- 107 KeInitializeDpc
- 109 KeInitializeInterrupt
- 113 KeInitializeTimerEx
- 119 KeInsertQueueDpc
- 128 KeQuerySystemTime
- 129 KeRaiseIrqlToDpcLevel
- 137 KeRemoveQueueDpc
- 139 KeRestoreFloatingPointState
- 142 KeSaveFloatingPointState
- 143 KeSetBasePriorityThread
- 145 KeSetEvent
- 149 KeSetTimer
- 151 KeStallExecutionProcessor
- 153 KeSynchronizeExecution
- 156 KeTickCount
- 159 KeWaitForSingleObject
- 160 KfRaiseIrql
- 161 KfLowerIrql

## Memory manager (12)
- 165 MmAllocateContiguousMemory
- 166 MmAllocateContiguousMemoryEx
- 168 MmClaimGpuInstanceMemory
- 171 MmFreeContiguousMemory
- 173 MmGetPhysicalAddress
- 175 MmLockUnlockBufferPages
- 176 MmLockUnlockPhysicalPage
- 178 MmPersistContiguousMemory
- 179 MmQueryAddressProtect
- 180 MmQueryAllocationSize
- 181 MmQueryStatistics
- 182 MmSetAddressProtect

## NT syscalls (files, threads, sync, memory) (28)
- 184 NtAllocateVirtualMemory
- 187 NtClose
- 189 NtCreateEvent
- 190 NtCreateFile
- 192 NtCreateMutant
- 196 NtDeviceIoControlFile
- 198 NtFlushBuffersFile
- 199 NtFreeVirtualMemory
- 200 NtFsControlFile
- 202 NtOpenFile
- 203 NtOpenSymbolicLinkObject
- 207 NtQueryDirectoryFile
- 210 NtQueryFullAttributesFile
- 211 NtQueryInformationFile
- 215 NtQuerySymbolicLinkObject
- 217 NtQueryVirtualMemory
- 218 NtQueryVolumeInformationFile
- 219 NtReadFile
- 221 NtReleaseMutant
- 224 NtResumeThread
- 225 NtSetEvent
- 226 NtSetInformationFile
- 231 NtSuspendThread
- 233 NtWaitForSingleObject
- 234 NtWaitForSingleObjectEx
- 235 NtWaitForMultipleObjectsEx
- 236 NtWriteFile
- 238 NtYieldExecution

## Object manager (2)
- 246 ObReferenceObjectByHandle
- 250 ObfDereferenceObject

## Runtime library (Rtl) (10)
- 269 RtlCompareMemoryUlong
- 277 RtlEnterCriticalSection
- 279 RtlEqualString
- 289 RtlInitAnsiString
- 291 RtlInitializeCriticalSection
- 294 RtlLeaveCriticalSection
- 301 RtlNtStatusToDosError
- 302 RtlRaiseException
- 308 RtlUnicodeStringToAnsiString
- 312 RtlUnwind

## Threads (Ps) (3)
- 255 PsCreateSystemThreadEx
- 258 PsTerminateSystemThread
- 259 PsThreadObjectType

## Video output (Av) (4)
- 1 AvGetSavedDataAddress
- 2 AvSendTVEncoderOption
- 3 AvSetDisplayMode
- 4 AvSetSavedDataAddress
