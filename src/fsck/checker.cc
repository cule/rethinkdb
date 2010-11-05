#include "fsck/checker.hpp"

#include "config/cmd_args.hpp"
#include "containers/segmented_vector.hpp"
#include "serializer/log/log_serializer.hpp"
#include "btree/key_value_store.hpp"  // TODO move serializer_config_block_t to its own file.

namespace fsck {

struct device_block {
    void *buf;
    device_block() : buf(malloc_aligned(DEVICE_BLOCK_SIZE, DEVICE_BLOCK_SIZE)) { }
    device_block(direct_file_t *file, off64_t offset) : buf(malloc_aligned(DEVICE_BLOCK_SIZE, DEVICE_BLOCK_SIZE)) {
        file->read_blocking(offset, DEVICE_BLOCK_SIZE, buf);
    }
    ~device_block() { free(buf); }
};

// Knowledge that we contain for a particular block id.
struct block_knowledge {
    // The offset found in the LBA.
    flagged_off64_t offset;
    
    // If known, what is the serializer transaction id (of the current
    // instance of the block)?  If not known, the value is
    // NULL_SER_TRANSACTION_ID.  If the value is known, that means
    // (for non-deleted blocks) that the block is "used," since we
    // update the value when we read the buffer at the above offset.
    ser_transaction_id_t transaction_id;
};

// The knowledge we have about a particular file is gathered here.
struct file_knowledge {
    // The serializer number, known by position on command line.
    // These values get happily overridden, with only a warning, by
    // serializer_config_block_t::this_serializer.
    int predicted_serializer_number;

    // The file size, known after we've looked at the file.
    uint64_t filesize;
    bool filesize_known;

    // The block size and extent size.
    log_serializer_static_config_t static_config;
    bool static_config_known;
    
    // The metablock with the most recent version.
    log_serializer_metablock_t metablock;
    bool metablock_known;

    // The block id to offset mapping.
    segmented_vector_t<flagged_off64_t, MAX_BLOCK_ID> block_offsets;
    bool block_offsets_known;

    // The block from CONFIG_BLOCK_ID (well, the beginning of such a block).
    serializer_config_block_t config_block;
    bool config_block_known;

    file_knowledge()
        : predicted_serializer_number(-1), filesize_known(false), static_config_known(false),
          metablock_known(false), block_offsets_known(false), config_block_known(false) { }

private:
    DISABLE_COPYING(file_knowledge);
};

// The knowledge we have is gathered here.
struct knowledge {
    const std::vector<std_string_t, gnew_alloc<std_string_t> > filenames;
    std::vector<direct_file_t *, gnew_alloc<direct_file_t *> > files;
    std::vector<file_knowledge *, gnew_alloc<file_knowledge *> > file_knog;

    knowledge(const std::vector<std_string_t, gnew_alloc<std_string_t> >& filenames)
        : filenames(filenames), files(filenames.size(), NULL), file_knog(filenames.size(), NULL) {
        int num_files = filenames.size();
        for (int i = 0; i < num_files; ++i) {
            direct_file_t *file = new direct_file_t(filenames[i].c_str(), direct_file_t::mode_read);
            files[i] = file;
            file_knog[i] = gnew<file_knowledge>();
            file_knog[i]->predicted_serializer_number = i;
        }
    }

    ~knowledge() {
        int num_files = filenames.size();
        for (int i = 0; i < num_files; ++i) {
            delete files[i];
            gdelete(file_knog[i]);
        }
    }

private:
    DISABLE_COPYING(knowledge);
};

void require_fact(bool fact, const char *test, const char *options, ...) {
    // TODO varargs
    if (!fact) {
        logERR("Checking '%s': FAIL\n", test);
        fail("ERROR: test '%s' failed!  To override, use options %s.", test, options);
    } else {
        logDBG("Checking '%s': PASS", test);
    }
}

void unrecoverable_fact(bool fact, const char *test, ...) {
    // TODO varargs
    if (!fact) {
        logERR("Checking '%s': FAIL\n", test);
        fail("ERROR: test '%s' failed!  Cannot override.", test);
    } else {
        logDBG("Checking '%s': PASS\n");
    }
}

void check_fact(bool fact, const char *msg, ...) {
    // TODO record errors.
    if (!fact) {
        logWRN("Checking '%s': FAIL\n", msg);
    }
}


void check_filesize(direct_file_t *file, file_knowledge *knog) {
    assert(!knog->filesize_known);
    knog->filesize = file->get_size();
    knog->filesize_known = true;
}

void check_static_config(direct_file_t *file, file_knowledge *knog) {
    assert(knog->filesize_known);

    /* Read the file static config.
         - MAGIC
         - CHECK block_size divides extent_size divides file_size.
         - LEARN block_size, extent_size. */
    device_block header(file, 0);
    static_header_t *buf = (static_header_t *)header.buf;
    

    log_serializer_static_config_t *static_cfg = (log_serializer_static_config_t *)(buf + 1);
    
    uint64_t block_size = static_cfg->block_size;
    uint64_t extent_size = static_cfg->extent_size;
    uint64_t file_size = knog->filesize;

    logINF("static_header software_name: %.*s\n", sizeof(SOFTWARE_NAME_STRING), buf->software_name);
    logINF("static_header version: %.*s\n", sizeof(VERSION_STRING), buf->version);
    logINF("              DEVICE_BLOCK_SIZE: %u\n", DEVICE_BLOCK_SIZE);
    logINF("static_header block_size: %lu\n", block_size);
    logINF("static_header extent_size: %lu\n", extent_size);
    logINF("              file_size: %lu\n", file_size);

    // MAGIC
    require_fact(!strcmp(buf->software_name, SOFTWARE_NAME_STRING),
                 "static_header software_name", "--ignore-static-header-magic");  // TODO option
    require_fact(!strcmp(buf->version, VERSION_STRING),
                 "static_header version", "--ignore-static-header-magic");

    // CHECK block_size divides extent_size divides file_size.

    // TODO option
    require_fact(block_size % DEVICE_BLOCK_SIZE == 0, "block_size % DEVICE_BLOCK_SIZE", "--force-block-size --force-extent-size");
    require_fact(extent_size % block_size == 0, "extent_size % block_size", "--force-block-size --force-extent-size");
    require_fact(file_size % extent_size == 0, "file_size % extent_size", "--force-block-size --force-extent-size");

    // LEARN block_size, extent_size.
    knog->static_config = *static_cfg;
    assert(!knog->static_config_known);
    knog->static_config_known = true;
}

void check_metablock(direct_file_t *file, file_knowledge *knog) {
    assert(knog->static_config_known);
    /* Read metablocks
         - MAGIC
         - CHECK metablock monotonicity.
         - LEARN lba_shard_metablock_t[0:LBA_SHARD_FACTOR].
    */

    std::vector<off64_t, gnew_alloc<off64_t> > metablock_offsets;
    initialize_metablock_offsets(knog->static_config.extent_size, &metablock_offsets);

    typedef metablock_manager_t<log_serializer_metablock_t> manager_t;
    typedef manager_t::crc_metablock_t crc_metablock_t;


    off64_t high_version_offset = -1;
    manager_t::metablock_version_t high_version = MB_START_VERSION - 1;

    off64_t high_transaction_offset = -1;
    ser_transaction_id_t high_transaction = NULL_SER_TRANSACTION_ID;


    for (size_t i = 0; i < metablock_offsets.size(); ++i) {
        off64_t off = metablock_offsets[i];
        device_block b(file, off);
        crc_metablock_t *metablock = (crc_metablock_t *)b.buf;

        if (metablock->check_crc()) {
            // MAGIC
            check_fact(!memcmp(metablock->magic_marker, MB_MARKER_MAGIC, sizeof(MB_MARKER_MAGIC))
                       && !memcmp(metablock->crc_marker, MB_MARKER_CRC, sizeof(MB_MARKER_CRC))
                       && !memcmp(metablock->version_marker, MB_MARKER_VERSION, sizeof(MB_MARKER_VERSION)),
                       "metablock with bad magic! (offset = %ld)", off);

            if (high_version < metablock->version) {
                high_version = metablock->version;
                high_version_offset = i;
            }

            if (high_transaction < metablock->metablock.transaction_id) {
                high_transaction = metablock->metablock.transaction_id;
                high_transaction_offset = i;
            }
        } else {
            bool all_zero = true;
            byte *buf = (byte *)b.buf;
            for (int i = 0; i < DEVICE_BLOCK_SIZE; ++i) {
                all_zero &= (buf[i] == 0);
            }
            check_fact(all_zero, "metablock with bad crc! (offset = %ld)", off);
        }
    }

    unrecoverable_fact(high_version_offset != -1, "Expecting some nonzero metablocks!");

    require_fact(high_version_offset == high_transaction_offset,
                 "metablocks' metablock_version_t and ser_transaction_id are not equally monotonic!",
                 "--tolerate-metablock-disorder");  // TODO option

    // Reread the best block, based on the metablock version.
    device_block high_block(file, high_version_offset);
    crc_metablock_t *high_metablock = (crc_metablock_t *)high_block.buf;

    knog->metablock = high_metablock->metablock;
    assert(!knog->metablock_known);
    knog->metablock_known = true;
}

void check_lba(direct_file_t *file, file_knowledge *knog) {
    // TODO    
}

void check_config_block(direct_file_t *file, file_knowledge *knog) {
    // TODO
}

void check_slice(direct_file_t *file, file_knowledge *knog, int global_slice_number) {
    // TODO
}

void check_garbage_block_inferiority(direct_file_t *file, file_knowledge *knog) {
    // TODO
    // TODO consider doing this slice by slice.
}

void check_to_config_block(direct_file_t *file, file_knowledge *knog) {
    check_filesize(file, knog);

    check_static_config(file, knog);

    check_metablock(file, knog);

    check_lba(file, knog);

    check_config_block(file, knog);
}

void check_config_blocks(knowledge *knog) {
    // TODO
}

void check_after_config_block(direct_file_t *file, file_knowledge *knog) {
    assert(knog->config_block_known);

    // TODO figure out which slices.

    for (int i = 0; i < knog->config_block.btree_config.n_slices; ++i) {
        check_slice(file, knog, i);  // TODO
    }

    check_garbage_block_inferiority(file, knog);  // TODO
}

void check_files(const fsck_config_t& cfg) {
    // 1. Open.
    knowledge knog(cfg.input_filenames);

    int num_files = knog.filenames.size();

    // 2. Read some (and check).  This could be parallelized.
    for (int i = 0; i < num_files; ++i) {
        check_to_config_block(knog.files[i], knog.file_knog[i]);
    }

    // 3. Check.
    check_config_blocks(&knog);

    // 4. Read some more (and check).  This could be parallelized.
    for (int i = 0; i < num_files; ++i) {
        check_after_config_block(knog.files[i], knog.file_knog[i]);
    }
}


}  // namespace fsck
