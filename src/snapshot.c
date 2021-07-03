#include <string.h>
#include <sys/ptrace.h>
#include <sys/user.h>
#include <sys/wait.h>
#include <unistd.h>

#include "scan.h"  // /proc/X/maps stuff
#define __SRCFILE__ "snapshot"
#include "memory.h"    // read_from_memory, write_to_memory
#include "snapshot.h"  // defines
#include "util.h"      // LOG

process_snapshot *snap = NULL;
void save_snapshot(pid_t pid) {
    // CHECK(snap != NULL, "save_snapshot called when we already have one!\n");
    if (snap != NULL) return;
#if DEBUG_SNAPSHOTS
    LOG("saving snapshot of pid %d\n", pid);
#endif
#if DEBUG_STOP_WHEN_SNAPPING
    LOG("before snapshot save\n");
    getchar();
#endif
    map_list *list = get_maps_for_pid(pid, PERM_RW);
    // print_list(list);
    map_entry *entry_list = list->entries;
    map_entry *cur_map_entry;

    snap = malloc(sizeof(process_snapshot));
    snap->area_count = list->len;
    snap->memory_stores = malloc(sizeof(snapshot_area) * snap->area_count);
    //    LOG("memory_stores is %p\n", snap->memory_stores);
    snapshot_area *cur_snap_area;
    for (size_t j = 0; j < list->len; j++) {
        cur_map_entry = &entry_list[j];
        cur_snap_area = &snap->memory_stores[j];
#if DEBUG_SNAPSHOTS
        LOG("snap->memory_stores is at %p\n", snap->memory_stores);
        LOG("snap->memory_stores[j] is at %p\n", &snap->memory_stores[j]);
        LOG("cur_snap_area is at %p\n", cur_snap_area);
#endif

        if (strcmp(cur_map_entry->path, "[vvar]") == 0 ||
            strcmp(cur_map_entry->path, "[vsyscall]") == 0 ||
            strcmp(cur_map_entry->path, "[vdso]") == 0) {
#if DEBUG_SNAPSHOTS
            LOG("skipping region %s\n", cur_map_entry->path);
#endif
            continue;
        }

        uint64_t sz = cur_map_entry->end - cur_map_entry->start;
        uintptr_t orig_addr = cur_map_entry->start;
        cur_snap_area->size = sz;
        cur_snap_area->original_address = orig_addr;

        if (strcmp(cur_map_entry->path, "[heap]") == 0) {
#if DEBUG_SNAPSHOTS
            LOG("saving original heap size as %p\n",
                (void *)cur_map_entry->end);
#endif
            snap->original_heap_size = cur_map_entry->end;
        }
        uint8_t *buf = malloc(sizeof(uint8_t) * sz);
        // NOLINTNEXTLINE(clang-analyzer-security.insecureAPI.DeprecatedOrUnsafeBufferHandling)
        memset(buf, 0, sizeof(uint8_t) * sz);
        cur_snap_area->backing = buf;
        CHECK(buf == NULL, "could not allocate space for snapshot\n");

#if DEBUG_SNAPSHOTS
        LOG("reading region %s[%p-%p, %lu] into snapshot\n",
            cur_map_entry->path, (void *)cur_map_entry->start,
            (void *)cur_map_entry->end, sz);
#endif
        ssize_t read = read_from_memory(pid, buf, orig_addr, sz);
#if 0
        LOG("our buffer, from readv\n");
        for (int x = 0; x < 64; x++) {
            if (x % 8 == 0) fprintf(stderr, "\n");
            fprintf(stderr, "%x ", buf[x]);
        }
        fprintf(stderr, "\n\n");
#endif
#if DEBUG_SNAPSHOTS
        LOG("size_t read is %lu, sz is %lu\n", read, sz);
#endif
        CHECK((size_t)read != sz, "did not read expected amount of memory!\n");
    }
#if DEBUG_SNAPSHOTS
    LOG("saving registers\n");
#endif
    int ret = ptrace(PTRACE_GETREGS, pid, NULL, &snap->regs);
    CHECK(ret == -1, "failed to get registers\n");
#if DEBUG_SNAPSHOTS
    LOG("at snapshot save, RIP is %p\n",
        (void *)snap->regs.rip);  // assuming 64-bit
#endif
    ret = ptrace(PTRACE_GETFPREGS, pid, NULL, &snap->fpregs);
    CHECK(ret == -1, "failed to get fp registers\n");
#if DEBUG_STOP_WHEN_SNAPPING
    LOG("after snapshot save\n");
    getchar();
    debug_regs_singlestep(pid, DEBUG_STEPS);
    getchar();
#endif
}

void restore_snapshot(pid_t pid, int TYPE) {
    CHECK(snap == NULL, "snapshot is null! none taken??\n");

#if DEBUG_SNAPSHOTS
    LOG("restoring snapshot of pid %d\n", pid);
    LOG("at snapshot restore, saved RIP is %p\n",
        (void *)snap->regs.rip);  // assuming 64-bit
#endif
#if DEBUG_STOP_WHEN_SNAPPING
    LOG("before restore\n");
    getchar();
#endif

    if (TYPE == RESTORE_MEMORY || TYPE == RESTORE_BOTH) {
        snapshot_area *cur_snap_area;
        // TODO: batch these into a single process_vm_writev
        for (size_t j = 0; j < snap->area_count; j++) {
            cur_snap_area = &snap->memory_stores[j];
            //        LOG("snap->memory_stores is at %p\n",
            //        snap->memory_stores); LOG("snap->memory_stores[j] is at
            //        %p\n", &snap->memory_stores[j]); LOG("cur_snap_area is at
            //        %p\n", cur_snap_area);

#if DEBUG_SNAPSHOTS
            LOG("writing %ld bytes\n", cur_snap_area->size);
            //            LOG("page: %s : %p : %s\n", cur_map_entry->path,
            /*              (void *)cur_map_entry->start, cur_map_entry->perms);
                      LOG("writing region %s[%p-%p, %lu] from snapshot\n(%lu
               bytes, from "
                          "%p "
                          "to %p)\n",
                          cur_map_entry->path, (void *)cur_map_entry->start,
                          (void *)cur_map_entry->end, cur_snap_area->size,
                          cur_snap_area->size, (void *)cur_snap_area->backing,
                          (void *)cur_snap_area->original_address);*/
#endif
            ssize_t written = write_to_memory(pid, cur_snap_area->backing,
                                              cur_snap_area->original_address,
                                              cur_snap_area->size);
            for (int i = 0; i < 10; i++) {
                written = write_to_memory(pid, cur_snap_area->backing,
                                          cur_snap_area->original_address,
                                          cur_snap_area->size);
                if (written == (ssize_t)cur_snap_area->size) break;
                LOG("did not write all bytes (%lu of %lu) retrying %d/10\n",
                    written, cur_snap_area->size, i);
            }
#if DEBUG_SNAPSHOTS
            for (int x = 0; x < 64; x++) {
                if (x % 8 == 0) fprintf(stderr, "\n");
                fprintf(stderr, "%x ", cur_snap_area->backing[x]);
            }
            fprintf(stderr, "\n\n");
#endif
            CHECK((size_t)written != cur_snap_area->size,
                  "did not write expected amount of memory!\n");
        }
    }
    if (TYPE == RESTORE_REGISTERS || TYPE == RESTORE_BOTH) {
#if DEBUG_SNAPSHOTS
        LOG("restoring registers\n");
#endif

        int ret = ptrace(PTRACE_SETREGS, pid, NULL, &snap->regs);
        CHECK(ret == -1, "failed to set registers\n");
        ret = ptrace(PTRACE_SETFPREGS, pid, NULL, &snap->fpregs);
        CHECK(ret == -1, "failed to set fp registers\n");
#if DEBUG_SNAPSHOTS
        struct user_regs_struct check_regs;
        ret = ptrace(PTRACE_GETREGS, pid, NULL, &check_regs);
        CHECK(ret == -1, "failed to get registers\n");
        LOG("new RIP: %p\n", (void *)check_regs.rip);
#endif
    }
#if DEBUG_STOP_WHEN_SNAPPING
    LOG("after restore\n");
    getchar();
    debug_regs_singlestep(pid, DEBUG_STEPS);
#endif
}

int have_snapshot() {
    if (snap == NULL) return NO_SNAPSHOT;
    return HAVE_SNAPSHOT;
}

void debug_regs_singlestep(pid_t pid, uint64_t steps) {
    map_list *list = get_maps_for_pid(pid, PERM_RW);
    print_list(list);
    struct user_regs_struct check_regs;
    for (uint64_t _ = 0; _ < steps; _++) {
        int ret = ptrace(PTRACE_SINGLESTEP, pid, NULL, NULL);
        CHECK(ret == -1, "failed to singlestep\n");
        int status;
        waitpid(pid, &status, 0);
        if (_ < DEBUG_STEPS_SKIP) continue;
        if (WIFSTOPPED(status)) {
            ret = ptrace(PTRACE_GETREGS, pid, NULL, &check_regs);
            CHECK(ret == -1, "failed to check registers\n");
            // userland: %rdi, %rsi, %rdx, %rcx, %r8 and %r9
            //   kernel: %rdi, %rsi, %rdx, %r10, %r8 and %r9
            LOG("step %d\nrip: %" PRIx64 "\nrdi: %" PRIx64 "\nrsi: %" PRIx64
                "\nrdx: %" PRIx64 "\nr10: %" PRIx64 "\n r8: %" PRIx64
                "\n r9: %" PRIx64 "\n cs: %" PRIx64 "\n eflags: %" PRIx64
                "\n rbx: %" PRIx64 "\n rbp: %" PRIx64 "\n",
                _, check_regs.rip, check_regs.rdi, check_regs.rsi,
                check_regs.rdx, check_regs.r10, check_regs.r8, check_regs.r9,
                check_regs.cs, check_regs.eflags, check_regs.rbx,
                check_regs.rbp);

        } else {
            CHECK(1, "oops\n");
        }
    }
}

#if 0
void dump_snapshot_info() {
    snapshot_area *stores = snap->memory_stores;
    LOG("\n -- SNAPSHOT INFO -- \nsnap stores addr: %p\nentries: %lu\n",
        (void *)snap->memory_stores, snap->area_count);
    snapshot_area *cur_store;
    for (uint8_t j = 0; j < snap->area_count; j++) {
        cur_store = &stores[j];
        //        if (j == 0 || j == 21) {
        LOG("cur store is %2d, cur_store=%p, sz: %lu, orig addr: %p, backing: "
            "%p\n",
            j, (void *)cur_store, cur_store->size,
            (void *)cur_store->original_address, (void *)cur_store->backing);
        //       }
    }
}
#endif
