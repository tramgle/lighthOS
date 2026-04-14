#include "test/test.h"
#include "fs/vfs.h"

test_results_t test_vfs(void) {
    TEST_SUITE_BEGIN("vfs");

    /* Root should be resolvable (mounted in main.c before tests run) */
    vfs_node_t *root = vfs_resolve("/");
    TEST_ASSERT_NEQ((uint32_t)root, 0, "vfs_resolve(\"/\") returns non-null");

    /* Create a test file */
    int rc = vfs_create("/test_file.txt", VFS_FILE);
    TEST_ASSERT_EQ(rc, 0, "vfs_create file succeeds");

    /* Stat it */
    struct vfs_stat st;
    rc = vfs_stat("/test_file.txt", &st);
    TEST_ASSERT_EQ(rc, 0, "vfs_stat on created file succeeds");
    TEST_ASSERT_EQ(st.type, VFS_FILE, "stat shows VFS_FILE type");
    TEST_ASSERT_EQ(st.size, 0, "new file has size 0");

    /* Write data */
    const char *data = "Hello VFS!";
    ssize_t written = vfs_write("/test_file.txt", data, 10, 0);
    TEST_ASSERT_EQ(written, 10, "vfs_write returns 10 bytes");

    /* Read it back */
    char buf[32];
    memset(buf, 0, sizeof(buf));
    ssize_t read = vfs_read("/test_file.txt", buf, 32, 0);
    TEST_ASSERT_EQ(read, 10, "vfs_read returns 10 bytes");
    TEST_ASSERT_STR_EQ(buf, "Hello VFS!", "vfs_read data matches written data");

    /* Stat shows updated size */
    rc = vfs_stat("/test_file.txt", &st);
    TEST_ASSERT_EQ(st.size, 10, "stat shows updated size after write");

    /* Create a directory */
    rc = vfs_mkdir("/test_dir");
    TEST_ASSERT_EQ(rc, 0, "vfs_mkdir succeeds");
    rc = vfs_stat("/test_dir", &st);
    TEST_ASSERT_EQ(st.type, VFS_DIR, "mkdir creates VFS_DIR");

    /* Readdir on root finds our entries */
    char name[64];
    uint32_t type;
    bool found_file = false, found_dir = false;
    for (uint32_t i = 0; vfs_readdir("/", i, name, &type) == 0; i++) {
        if (strcmp(name, "test_file.txt") == 0) found_file = true;
        if (strcmp(name, "test_dir") == 0) found_dir = true;
    }
    TEST_ASSERT(found_file, "readdir finds test_file.txt");
    TEST_ASSERT(found_dir, "readdir finds test_dir");

    /* Unlink file */
    rc = vfs_unlink("/test_file.txt");
    TEST_ASSERT_EQ(rc, 0, "vfs_unlink succeeds");
    rc = vfs_stat("/test_file.txt", &st);
    TEST_ASSERT_NEQ(rc, 0, "stat fails after unlink");

    /* Clean up directory */
    vfs_unlink("/test_dir");

    TEST_SUITE_END();
}
