# Capturing a Windows illegal-instruction address

Use this when CrispASR exits with `-1073741795` (`0xC000001D`). The exit code
only says that Windows raised `STATUS_ILLEGAL_INSTRUCTION`; the dump identifies
the exact instruction and its offset within `crispasr.exe` or a DLL.

For #302, first run the fixed binary normally. If it starts, repeat once from a
Command Prompt with the old eager behavior restored:

```bat
set GGML_CPU_EAGER_UE4M3_LUT=1
rem launch the same crispasr.exe command here
```

Default-starts/eager-crashes is a controlled confirmation that the UE4M3
initialization was the trigger. If the default still crashes, capture its dump
as below; a different optimized CPU path is executing during startup.

## Capture a minimal dump

1. Download Microsoft's Sysinternals ProcDump and create `C:\crispasr-dumps`.
2. Open Command Prompt and run this *before* launching CrispASR:

   ```bat
   procdump64.exe -accepteula -e -w crispasr.exe C:\crispasr-dumps
   ```

3. Reproduce the crash once. ProcDump's default mini dump includes the exception
   context, thread stacks, module list, and referenced memory. Do not use `-ma`
   unless requested: a full dump may contain model data, audio, paths, and other
   process memory.
4. Record the exact CrispASR release/filename and the CPU name:

   ```powershell
   Get-CimInstance Win32_Processor | Select-Object -ExpandProperty Name
   ```

## Extract the useful address locally

Open the `.dmp` in WinDbg and run:

```text
.logopen /t crispasr-illegal-instruction.txt
!analyze -v
.exr -1
.ecxr
r
ub @rip L8
u @rip L8
k
lm
.logclose
```

Send `crispasr-illegal-instruction*.txt`, the CPU name, and the exact binary—not
the dump—in the public issue. The key fields are the exception address, the
module containing `RIP`, the disassembled instruction at `RIP`, and the module
load address. Those let maintainers calculate an ASLR-independent module offset
and resolve it against the matching executable/PDB.

If the text is insufficient, share the `.dmp` privately. Even a mini dump can
contain local paths or small referenced buffers, so do not attach it publicly
without reviewing that risk.
