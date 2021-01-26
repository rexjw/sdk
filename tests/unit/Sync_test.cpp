/**
 * (c) 2019 by Mega Limited, Wellsford, New Zealand
 *
 * This file is part of the MEGA SDK - Client Access Engine.
 *
 * Applications using the MEGA API must present a valid application key
 * and comply with the the rules set forth in the Terms of Service.
 *
 * The MEGA SDK is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 *
 * @copyright Simplified (2-clause) BSD License.
 *
 * You should have received a copy of the license along with this
 * program.
 */

#include <memory>
#include <numeric>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <mega/megaclient.h>
#include <mega/megaapp.h>
#include <mega/types.h>
#include <mega/heartbeats.h>
#include <mega/sync.h>
#include <mega/filesystem.h>

#include "constants.h"
#include "FsNode.h"
#include "DefaultedDbTable.h"
#include "DefaultedDirAccess.h"
#include "DefaultedFileAccess.h"
#include "DefaultedFileSystemAccess.h"
#include "utils.h"

#ifdef ENABLE_SYNC

namespace {

class MockApp : public mega::MegaApp
{
public:

    bool sync_syncable(mega::Sync*, const char*, mega::LocalPath& localpath) override
    {
        return mNotSyncablePaths.find(localpath) == mNotSyncablePaths.end();
    }

    bool sync_syncable(mega::Sync*, const char*, mega::LocalPath& localpath, mega::Node*) override
    {
        return mNotSyncablePaths.find(localpath) == mNotSyncablePaths.end();
    }

    void addNotSyncablePath(const mega::LocalPath& path)
    {
        mNotSyncablePaths.insert(path);
    }

private:
    std::set<mega::LocalPath> mNotSyncablePaths;
};

class MockFileAccess : public mt::DefaultedFileAccess
{
public:
    explicit MockFileAccess(std::map<mega::LocalPath, const mt::FsNode*>& fsNodes)
    : mFsNodes{fsNodes}
    {}

    ~MockFileAccess()
    {
        assert(sOpenFileCount <= 2); // Ensure there's not more than two files open at a time
        if (mOpen)
        {
            --sOpenFileCount;
        }
    }

    MEGA_DISABLE_COPY_MOVE(MockFileAccess)

    bool fopen(mega::LocalPath& path, bool, bool, mega::DirAccess* iteratingDir, bool) override
    {
        mPath = path;
        return sysopen();
    }

    bool sysstat(mega::m_time_t* curr_mtime, m_off_t* curr_size) override
    {
        *curr_mtime = mtime;
        *curr_size = size;
        return true;
    }

    bool sysopen(bool async = false) override
    {
        const auto fsNodePair = mFsNodes.find(mPath);
        if (fsNodePair != mFsNodes.end())
        {
            mCurrentFsNode = fsNodePair->second;
            if (!mCurrentFsNode->getOpenable())
            {
                return false;
            }
            fsid = mCurrentFsNode->getFsId();
            fsidvalid = fsid != mega::UNDEF;
            size = mCurrentFsNode->getSize();
            mtime = mCurrentFsNode->getMTime();
            type = mCurrentFsNode->getType();
            mOpen = true;
            ++sOpenFileCount;
            return true;
        }
        else
        {
            return false;
        }
    }

    bool sysread(mega::byte* buffer, unsigned size, m_off_t offset) override
    {
        assert(mOpen);
        assert(mCurrentFsNode);
        if (!mCurrentFsNode->getReadable())
        {
            return false;
        }
        const auto& content = mCurrentFsNode->getContent();
        assert(static_cast<unsigned>(offset) + size <= content.size());
        std::copy(content.begin() + static_cast<unsigned>(offset), content.begin() + static_cast<unsigned>(offset) + size, buffer);
        return true;
    }

    void sysclose() override
    {}

private:
    static int sOpenFileCount;
    mega::LocalPath mPath;
    bool mOpen = false;
    const mt::FsNode* mCurrentFsNode{};
    std::map<mega::LocalPath, const mt::FsNode*>& mFsNodes;
};

int MockFileAccess::sOpenFileCount{0};

class MockDirAccess : public mt::DefaultedDirAccess
{
public:
    explicit MockDirAccess(std::map<mega::LocalPath, const mt::FsNode*>& fsNodes)
    : mFsNodes{fsNodes}
    {}

    MEGA_DISABLE_COPY_MOVE(MockDirAccess)

    bool dopen(mega::LocalPath* path, mega::FileAccess* fa, bool) override
    {
        assert(fa->type == mega::FOLDERNODE);
        const auto fsNodePair = mFsNodes.find(*path);
        if (fsNodePair != mFsNodes.end())
        {
            mCurrentFsNode = fsNodePair->second;
            return mCurrentFsNode->getOpenable();
        }
        else
        {
            return false;
        }
    }

    bool dnext(mega::LocalPath& localpath, mega::LocalPath& localname, bool = true, mega::nodetype_t* = NULL) override
    {
        assert(mCurrentFsNode);
        assert(mCurrentFsNode->getPath() == localpath);
        const auto& children = mCurrentFsNode->getChildren();
        if (mCurrentChildIndex < children.size())
        {
            localname = children[mCurrentChildIndex]->getName();
            ++mCurrentChildIndex;
            return true;
        }
        else
        {
            mCurrentChildIndex = 0;
            mCurrentFsNode = nullptr;
            return false;
        }
    }

private:
    const mt::FsNode* mCurrentFsNode{};
    std::size_t mCurrentChildIndex{};
    std::map<mega::LocalPath, const mt::FsNode*>& mFsNodes;
};

class MockFileSystemAccess : public mt::DefaultedFileSystemAccess
{
public:
    explicit MockFileSystemAccess(std::map<mega::LocalPath, const mt::FsNode*>& fsNodes)
    : mFsNodes{fsNodes}
    {}

    std::unique_ptr<mega::FileAccess> newfileaccess(bool) override
    {
        return std::unique_ptr<mega::FileAccess>{new MockFileAccess{mFsNodes}};
    }

    mega::DirAccess* newdiraccess() override
    {
        return new MockDirAccess{mFsNodes};
    }

    void local2path(const std::string* local, std::string* path) const override
    {
        *path = *local;
    }

    void path2local(const std::string* local, std::string* path) const override
    {
        *path = *local;
    }

    bool getsname(const mega::LocalPath&, mega::LocalPath&) const override
    {
        return false;
    }

private:
    std::map<mega::LocalPath, const mt::FsNode*>& mFsNodes;
};

struct Fixture
{
    explicit Fixture(std::string localname)
    : mUnifiedSync{mt::makeSync(*mClient, std::move(localname))}
    , mSync{mUnifiedSync->mSync}
    {}

    MEGA_DISABLE_COPY_MOVE(Fixture)

    MockApp mApp;
    std::map<mega::LocalPath, const mt::FsNode*> mFsNodes;
    MockFileSystemAccess mFsAccess{mFsNodes};
    std::shared_ptr<mega::MegaClient> mClient = mt::makeClient(mApp, mFsAccess);
    mega::handlelocalnode_map& mLocalNodes = mClient->fsidnode;
    std::unique_ptr<mega::UnifiedSync> mUnifiedSync;
    std::unique_ptr<mega::Sync>& mSync;

    bool iteratorsCorrect(mega::LocalNode& l) const
    {
        if (l.fsid_it == mLocalNodes.end())
        {
            return false;
        }
        auto localNodePair = mLocalNodes.find(l.fsid);
        if (l.fsid_it != localNodePair)
        {
            return false;
        }
        if (&l != localNodePair->second)
        {
            return false;
        }
        return true;
    }
};

}

namespace {

using mega::LocalPath;
using std::string;

/*
 * Shim to make following test less painful.
 */
int computeReversePathMatchScore(const string& path1,
                                 const string& path2)
{
#if defined(_WIN32)
    mega::WinFileSystemAccess wfa;
    auto localpath1 = LocalPath::fromPath(path1, wfa);
    auto localpath2 = LocalPath::fromPath(path2, wfa);

    return mega::computeReversePathMatchScore(localpath1,
                                              localpath2,
                                              mt::DefaultedFileSystemAccess());
#else
    return mega::computeReversePathMatchScore(
        LocalPath::fromPlatformEncoded(path1),
        LocalPath::fromPlatformEncoded(path2),
        mt::DefaultedFileSystemAccess());

#endif
}

void test_computeReversePathMatchScore()
{
    string sepstr;
    sepstr.push_back(LocalPath::localPathSeparator);
    ASSERT_EQ(0, computeReversePathMatchScore("", ""));
    ASSERT_EQ(0, computeReversePathMatchScore("", sepstr + "a"));
    ASSERT_EQ(0, computeReversePathMatchScore(sepstr + "b", ""));
    ASSERT_EQ(0, computeReversePathMatchScore("a", "b"));
    ASSERT_EQ(2, computeReversePathMatchScore("cc", "cc"));
    ASSERT_EQ(0, computeReversePathMatchScore(sepstr, sepstr));
    ASSERT_EQ(0, computeReversePathMatchScore(sepstr + "b", sepstr + "a"));
    ASSERT_EQ(2, computeReversePathMatchScore(sepstr + "cc", sepstr + "cc"));
    ASSERT_EQ(0, computeReversePathMatchScore(sepstr + "b", sepstr + "b" + sepstr));
    ASSERT_EQ(2, computeReversePathMatchScore(sepstr + "a" + sepstr + "b", sepstr + "a" + sepstr + "b"));
    ASSERT_EQ(2, computeReversePathMatchScore(sepstr + "a" + sepstr + "c" + sepstr + "a" + sepstr + "b", sepstr + "a" + sepstr + "b"));
    ASSERT_EQ(3, computeReversePathMatchScore(sepstr + "aaa" + sepstr + "bbbb" + sepstr + "ccc", sepstr + "aaa" + sepstr + "bbb" + sepstr + "ccc"));
    ASSERT_EQ(2, computeReversePathMatchScore("a" + sepstr + "b", "a" + sepstr + "b"));

    const string base = sepstr + "a" + sepstr + "b";
    const string reference = sepstr + "c12" + sepstr + "e34";

    ASSERT_EQ(6, computeReversePathMatchScore(base + reference, base + sepstr + "a65" + reference));
    ASSERT_EQ(6, computeReversePathMatchScore(base + reference, base + sepstr + ".debris" + reference));
    ASSERT_EQ(6, computeReversePathMatchScore(base + reference, base + sepstr + "ab" + reference));
}

}

TEST(Sync, computeReverseMatchScore_oneByteSeparator)
{
    test_computeReversePathMatchScore();
}

/*TEST(Sync, assignFilesystemIds_whenFilesystemFingerprintsMatchLocalNodes)
{
    Fixture fx{"d"};

    // Level 0
    mt::FsNode d{nullptr, mega::FOLDERNODE, "d"};
    mega::LocalNode& ld = *fx.mSync->localroot;
    static_cast<mega::FileFingerprint&>(ld) = d.getFingerprint();

    // Level 1
    mt::FsNode d_0{&d, mega::FOLDERNODE, "d_0"};
    auto ld_0 = mt::makeLocalNode(*fx.mSync, ld, mega::FOLDERNODE, "d_0", d_0.getFingerprint());
    mt::FsNode d_1{&d, mega::FOLDERNODE, "d_1"};
    auto ld_1 = mt::makeLocalNode(*fx.mSync, ld, mega::FOLDERNODE, "d_1", d_1.getFingerprint());
    mt::FsNode f_2{&d, mega::FILENODE, "f_2"};
    auto lf_2 = mt::makeLocalNode(*fx.mSync, ld, mega::FILENODE, "f_2", f_2.getFingerprint());

    // Level 2
    mt::FsNode f_0_0{&d_0, mega::FILENODE, "f_0_0"};
    auto lf_0_0 = mt::makeLocalNode(*fx.mSync, *ld_0, mega::FILENODE, "f_0_0", f_0_0.getFingerprint());
    mt::FsNode f_0_1{&d_0, mega::FILENODE, "f_0_1"};
    auto lf_0_1 = mt::makeLocalNode(*fx.mSync, *ld_0, mega::FILENODE, "f_0_1", f_0_1.getFingerprint());
    mt::FsNode f_1_0{&d_1, mega::FILENODE, "f_1_0"};
    auto lf_1_0 = mt::makeLocalNode(*fx.mSync, *ld_1, mega::FILENODE, "f_1_0", f_1_0.getFingerprint());
    mt::FsNode d_1_1{&d_1, mega::FOLDERNODE, "d_1_1"};
    auto ld_1_1 = mt::makeLocalNode(*fx.mSync, *ld_1, mega::FOLDERNODE, "d_1_1", d_1_1.getFingerprint());

    // Level 3
    mt::FsNode f_1_1_0{&d_1_1, mega::FILENODE, "f_1_1_0"};
    auto lf_1_1_0 = mt::makeLocalNode(*fx.mSync, *ld_1_1, mega::FILENODE, "f_1_1_0", f_1_1_0.getFingerprint());

    mt::collectAllFsNodes(fx.mFsNodes, d);

    auto localdebris = LocalPath::fromPath("d/" + mt::gLocalDebris, fx.mFsAccess);
    const auto success = mega::assignFilesystemIds(*fx.mSync, fx.mApp, fx.mFsAccess, fx.mLocalNodes, localdebris);

    ASSERT_TRUE(success);

    // assert that directories have correct fs IDs
    ASSERT_EQ(mega::UNDEF, ld.fsid);
    ASSERT_EQ(d_0.getFsId(), ld_0->fsid);
    ASSERT_EQ(d_1.getFsId(), ld_1->fsid);
    ASSERT_EQ(d_1_1.getFsId(), ld_1_1->fsid);

    // assert that all file `LocalNode`s have same fs IDs as the corresponding `FsNode`s
    ASSERT_EQ(f_2.getFsId(), lf_2->fsid);
    ASSERT_EQ(f_0_0.getFsId(), lf_0_0->fsid);
    ASSERT_EQ(f_0_1.getFsId(), lf_0_1->fsid);
    ASSERT_EQ(f_1_0.getFsId(), lf_1_0->fsid);
    ASSERT_EQ(f_1_1_0.getFsId(), lf_1_1_0->fsid);

    // assert that the local node map is correct
    constexpr std::size_t fileCount = 8;
    ASSERT_EQ(fileCount, fx.mLocalNodes.size());

    ASSERT_FALSE(fx.iteratorsCorrect(ld));
    ASSERT_TRUE(fx.iteratorsCorrect(*ld_0));
    ASSERT_TRUE(fx.iteratorsCorrect(*ld_1));
    ASSERT_TRUE(fx.iteratorsCorrect(*ld_1_1));
    ASSERT_TRUE(fx.iteratorsCorrect(*lf_2));
    ASSERT_TRUE(fx.iteratorsCorrect(*lf_0_0));
    ASSERT_TRUE(fx.iteratorsCorrect(*lf_0_1));
    ASSERT_TRUE(fx.iteratorsCorrect(*lf_1_0));
    ASSERT_TRUE(fx.iteratorsCorrect(*lf_1_1_0));
}

TEST(Sync, assignFilesystemIds_whenFilesystemFingerprintsMatchLocalNodes_oppositeDeclarationOrder)
{
    Fixture fx{"d"};

    // Level 0
    mt::FsNode d{nullptr, mega::FOLDERNODE, "d"};
    mega::LocalNode& ld = *fx.mSync->localroot;
    static_cast<mega::FileFingerprint&>(ld) = d.getFingerprint();

    // Level 1
    mt::FsNode d_0{&d, mega::FOLDERNODE, "d_0"};
    auto ld_0 = mt::makeLocalNode(*fx.mSync, ld, mega::FOLDERNODE, "d_0", d_0.getFingerprint());

    // Level 2

    // reverse order of declaration should still lead to same results (files vs localnodes)
    mt::FsNode f_0_1{&d_0, mega::FILENODE, "f_0_1"};
    mt::FsNode f_0_0{&d_0, mega::FILENODE, "f_0_0"};
    auto lf_0_0 = mt::makeLocalNode(*fx.mSync, *ld_0, mega::FILENODE, "f_0_0", f_0_0.getFingerprint());
    auto lf_0_1 = mt::makeLocalNode(*fx.mSync, *ld_0, mega::FILENODE, "f_0_1", f_0_1.getFingerprint());

    mt::collectAllFsNodes(fx.mFsNodes, d);

    auto localdebris = LocalPath::fromPath("d/" + mt::gLocalDebris, fx.mFsAccess);
    const auto success = mega::assignFilesystemIds(*fx.mSync, fx.mApp, fx.mFsAccess, fx.mLocalNodes, localdebris);

    ASSERT_TRUE(success);

    // assert that directories have correct fs IDs
    ASSERT_EQ(mega::UNDEF, ld.fsid);
    ASSERT_EQ(d_0.getFsId(), ld_0->fsid);

    // assert that all file `LocalNode`s have same fs IDs as the corresponding `FsNode`s
    ASSERT_EQ(f_0_0.getFsId(), lf_0_0->fsid);
    ASSERT_EQ(f_0_1.getFsId(), lf_0_1->fsid);

    // assert that the local node map is correct
    constexpr std::size_t fileCount = 3;
    ASSERT_EQ(fileCount, fx.mLocalNodes.size());

    ASSERT_FALSE(fx.iteratorsCorrect(ld));
    ASSERT_TRUE(fx.iteratorsCorrect(*ld_0));
    ASSERT_TRUE(fx.iteratorsCorrect(*lf_0_0));
    ASSERT_TRUE(fx.iteratorsCorrect(*lf_0_1));
}

TEST(Sync, assignFilesystemIds_whenNoLocalNodesMatchFilesystemFingerprints)
{
    Fixture fx{"d"};

    // Level 0
    mt::FsNode d{nullptr, mega::FOLDERNODE, "d"};
    mega::LocalNode& ld = *fx.mSync->localroot;

    // Level 1
    mt::FsNode d_0{&d, mega::FOLDERNODE, "d_0"};
    auto ld_0 = mt::makeLocalNode(*fx.mSync, ld, mega::FOLDERNODE, "d_0");
    mt::FsNode d_1{&d, mega::FOLDERNODE, "d_1"};
    auto ld_1 = mt::makeLocalNode(*fx.mSync, ld, mega::FOLDERNODE, "d_1");
    mt::FsNode f_2{&d, mega::FILENODE, "f_2"};
    auto lf_2 = mt::makeLocalNode(*fx.mSync, ld, mega::FILENODE, "f_2");

    // Level 2
    mt::FsNode f_0_0{&d_0, mega::FILENODE, "f_0_0"};
    auto lf_0_0 = mt::makeLocalNode(*fx.mSync, *ld_0, mega::FILENODE, "f_0_0");
    mt::FsNode f_0_1{&d_0, mega::FILENODE, "f_0_1"};
    auto lf_0_1 = mt::makeLocalNode(*fx.mSync, *ld_0, mega::FILENODE, "f_0_1");
    mt::FsNode f_1_0{&d_1, mega::FILENODE, "f_1_0"};
    auto lf_1_0 = mt::makeLocalNode(*fx.mSync, *ld_1, mega::FILENODE, "f_1_0");
    mt::FsNode d_1_1{&d_1, mega::FOLDERNODE, "d_1_1"};
    auto ld_1_1 = mt::makeLocalNode(*fx.mSync, *ld_1, mega::FOLDERNODE, "d_1_1");

    // Level 3
    mt::FsNode f_1_1_0{&d_1_1, mega::FILENODE, "f_1_1_0"};
    auto lf_1_1_0 = mt::makeLocalNode(*fx.mSync, *ld_1_1, mega::FILENODE, "f_1_1_0");

    mt::collectAllFsNodes(fx.mFsNodes, d);

    auto localdebris = LocalPath::fromPath("d/" + mt::gLocalDebris, fx.mFsAccess);
    const auto success = mega::assignFilesystemIds(*fx.mSync, fx.mApp, fx.mFsAccess, fx.mLocalNodes, localdebris);

    ASSERT_TRUE(success);

    // assert that files and directories have invalid fs IDs (no fingerprint matches)
    ASSERT_EQ(mega::UNDEF, ld.fsid);
    ASSERT_EQ(mega::UNDEF, ld_0->fsid);
    ASSERT_EQ(mega::UNDEF, ld_1->fsid);
    ASSERT_EQ(mega::UNDEF, ld_1_1->fsid);
    ASSERT_EQ(mega::UNDEF, lf_2->fsid);
    ASSERT_EQ(mega::UNDEF, lf_0_0->fsid);
    ASSERT_EQ(mega::UNDEF, lf_0_1->fsid);
    ASSERT_EQ(mega::UNDEF, lf_1_0->fsid);
    ASSERT_EQ(mega::UNDEF, lf_1_1_0->fsid);
}

TEST(Sync, assignFilesystemIds_whenTwoLocalNodesHaveSameFingerprint)
{
    Fixture fx{"d"};

    // Level 0
    mt::FsNode d{nullptr, mega::FOLDERNODE, "d"};
    mega::LocalNode& ld = *fx.mSync->localroot;
    static_cast<mega::FileFingerprint&>(ld) = d.getFingerprint();

    // Level 1
    mt::FsNode d_0{&d, mega::FOLDERNODE, "d_0"};
    auto ld_0 = mt::makeLocalNode(*fx.mSync, ld, mega::FOLDERNODE, "d_0", d_0.getFingerprint());
    mt::FsNode d_1{&d, mega::FOLDERNODE, "d_1"};
    auto ld_1 = mt::makeLocalNode(*fx.mSync, ld, mega::FOLDERNODE, "d_1", d_1.getFingerprint());
    mt::FsNode f_2{&d, mega::FILENODE, "f_2"};
    auto lf_2 = mt::makeLocalNode(*fx.mSync, ld, mega::FILENODE, "f_2", f_2.getFingerprint());

    // Level 2
    mt::FsNode f_0_0{&d_0, mega::FILENODE, "f_0_0"};
    auto lf_0_0 = mt::makeLocalNode(*fx.mSync, *ld_0, mega::FILENODE, "f_0_0", f_0_0.getFingerprint());
    mt::FsNode f_0_1{&d_0, mega::FILENODE, "f_0_1"};
    auto lf_0_1 = mt::makeLocalNode(*fx.mSync, *ld_0, mega::FILENODE, "f_0_1", f_0_1.getFingerprint());
    mt::FsNode f_1_0{&d_1, mega::FILENODE, "f_1_0"};
    auto lf_1_0 = mt::makeLocalNode(*fx.mSync, *ld_1, mega::FILENODE, "f_1_0", f_1_0.getFingerprint());
    mt::FsNode d_1_1{&d_1, mega::FOLDERNODE, "d_1_1"};
    auto ld_1_1 = mt::makeLocalNode(*fx.mSync, *ld_1, mega::FOLDERNODE, "d_1_1", d_1_1.getFingerprint());

    // Level 3
    mt::FsNode f_1_1_0{&d_1_1, mega::FILENODE, "f_1_1_0"};
    f_1_1_0.assignContentFrom(f_1_0);
    auto lf_1_1_0 = mt::makeLocalNode(*fx.mSync, *ld_1_1, mega::FILENODE, "f_1_1_0", f_1_1_0.getFingerprint());

    mt::collectAllFsNodes(fx.mFsNodes, d);

    auto localdebris = LocalPath::fromPath("d/" + mt::gLocalDebris, fx.mFsAccess);
    const auto success = mega::assignFilesystemIds(*fx.mSync, fx.mApp, fx.mFsAccess, fx.mLocalNodes, localdebris);

    ASSERT_TRUE(success);

    // assert that directories have correct fs IDs
    ASSERT_EQ(mega::UNDEF, ld.fsid);
    ASSERT_EQ(d_0.getFsId(), ld_0->fsid);
    ASSERT_EQ(d_1.getFsId(), ld_1->fsid);
    ASSERT_EQ(d_1_1.getFsId(), ld_1_1->fsid);

    // assert that all file `LocalNode`s have same fs IDs as the corresponding `FsNode`s
    ASSERT_EQ(f_2.getFsId(), lf_2->fsid);
    ASSERT_EQ(f_0_0.getFsId(), lf_0_0->fsid);
    ASSERT_EQ(f_0_1.getFsId(), lf_0_1->fsid);
    ASSERT_EQ(f_1_0.getFsId(), lf_1_0->fsid);
    ASSERT_EQ(f_1_1_0.getFsId(), lf_1_1_0->fsid);

    // assert that the local node map is correct
    constexpr std::size_t fileCount = 8;
    ASSERT_EQ(fileCount, fx.mLocalNodes.size());

    ASSERT_FALSE(fx.iteratorsCorrect(ld));
    ASSERT_TRUE(fx.iteratorsCorrect(*ld_0));
    ASSERT_TRUE(fx.iteratorsCorrect(*ld_1));
    ASSERT_TRUE(fx.iteratorsCorrect(*ld_1_1));
    ASSERT_TRUE(fx.iteratorsCorrect(*lf_2));
    ASSERT_TRUE(fx.iteratorsCorrect(*lf_0_0));
    ASSERT_TRUE(fx.iteratorsCorrect(*lf_0_1));
    ASSERT_TRUE(fx.iteratorsCorrect(*lf_1_0));
    ASSERT_TRUE(fx.iteratorsCorrect(*lf_1_1_0));
}

TEST(Sync, assignFilesystemIds_whenSomeFsIdIsNotValid)
{
    Fixture fx{"d"};

    // Level 0
    mt::FsNode d{nullptr, mega::FOLDERNODE, "d"};
    mega::LocalNode& ld = *fx.mSync->localroot;
    static_cast<mega::FileFingerprint&>(ld) = d.getFingerprint();

    // Level 1
    mt::FsNode f_0{&d, mega::FILENODE, "f_0"};
    f_0.setFsId(mega::UNDEF);
    auto lf_0 = mt::makeLocalNode(*fx.mSync, ld, mega::FILENODE, "f_0", f_0.getFingerprint());

    mt::collectAllFsNodes(fx.mFsNodes, d);

    auto localdebris = LocalPath::fromPath("d/" + mt::gLocalDebris, fx.mFsAccess);
    const auto success = mega::assignFilesystemIds(*fx.mSync, fx.mApp, fx.mFsAccess, fx.mLocalNodes, localdebris);

    ASSERT_FALSE(success);

    // assert that directories have correct fs IDs
    ASSERT_EQ(mega::UNDEF, ld.fsid);

    // file node must have undef fs ID
    ASSERT_EQ(mega::UNDEF, lf_0->fsid);

    // assert that the local node map is correct
    constexpr std::size_t fileCount = 0;
    ASSERT_EQ(fileCount, fx.mLocalNodes.size());

    ASSERT_FALSE(fx.iteratorsCorrect(ld));
    ASSERT_FALSE(fx.iteratorsCorrect(*lf_0));
}

TEST(Sync, assignFilesystemIds_whenSomeFileCannotBeOpened)
{
    Fixture fx{"d"};

    // Level 0
    mt::FsNode d{nullptr, mega::FOLDERNODE, "d"};
    mega::LocalNode& ld = *fx.mSync->localroot;
    static_cast<mega::FileFingerprint&>(ld) = d.getFingerprint();

    // Level 1
    mt::FsNode f_0{&d, mega::FILENODE, "f_0"};
    f_0.setOpenable(false);
    auto lf_0 = mt::makeLocalNode(*fx.mSync, ld, mega::FILENODE, "f_0", f_0.getFingerprint());

    mt::collectAllFsNodes(fx.mFsNodes, d);

    auto localdebris = LocalPath::fromPath("d/" + mt::gLocalDebris, fx.mFsAccess);
    const auto success = mega::assignFilesystemIds(*fx.mSync, fx.mApp, fx.mFsAccess, fx.mLocalNodes, localdebris);

    ASSERT_FALSE(success);
}

TEST(Sync, assignFilesystemIds_whenRootDirCannotBeOpened)
{
    Fixture fx{"d"};

    // Level 0
    mt::FsNode d{nullptr, mega::FOLDERNODE, "d"};
    d.setOpenable(false);
    mega::LocalNode& ld = *fx.mSync->localroot;
    static_cast<mega::FileFingerprint&>(ld) = d.getFingerprint();

    // Level 1
    mt::FsNode f_0{&d, mega::FILENODE, "f_0"};
    auto lf_0 = mt::makeLocalNode(*fx.mSync, ld, mega::FILENODE, "f_0", f_0.getFingerprint());

    mt::collectAllFsNodes(fx.mFsNodes, d);

    auto localdebris = LocalPath::fromPath("d/" + mt::gLocalDebris, fx.mFsAccess);
    const auto success = mega::assignFilesystemIds(*fx.mSync, fx.mApp, fx.mFsAccess, fx.mLocalNodes, localdebris);

    ASSERT_FALSE(success);
}

TEST(Sync, assignFilesystemIds_whenSubDirCannotBeOpened)
{
    Fixture fx{"d"};

    // Level 0
    mt::FsNode d{nullptr, mega::FOLDERNODE, "d"};
    mega::LocalNode& ld = *fx.mSync->localroot;
    static_cast<mega::FileFingerprint&>(ld) = d.getFingerprint();

    // Level 1
    mt::FsNode f_0{&d, mega::FILENODE, "f_0"};
    auto lf_0 = mt::makeLocalNode(*fx.mSync, ld, mega::FILENODE, "f_0", f_0.getFingerprint());
    mt::FsNode d_0{&d, mega::FOLDERNODE, "d_0"};
    d_0.setOpenable(false);
    auto ld_0 = mt::makeLocalNode(*fx.mSync, ld, mega::FOLDERNODE, "d_0", d_0.getFingerprint());

    mt::collectAllFsNodes(fx.mFsNodes, d);

    auto localdebris = LocalPath::fromPath("d/" + mt::gLocalDebris, fx.mFsAccess);
    const auto success = mega::assignFilesystemIds(*fx.mSync, fx.mApp, fx.mFsAccess, fx.mLocalNodes, localdebris);

    ASSERT_FALSE(success);

    // assert that directories have invalid fs IDs
    ASSERT_EQ(mega::UNDEF, ld.fsid);
    ASSERT_EQ(mega::UNDEF, ld_0->fsid);

    // check file nodes
    ASSERT_EQ(f_0.getFsId(), lf_0->fsid);

    // assert that the local node map is correct
    constexpr std::size_t fileCount = 1;
    ASSERT_EQ(fileCount, fx.mLocalNodes.size());

    ASSERT_FALSE(fx.iteratorsCorrect(ld));
    ASSERT_TRUE(fx.iteratorsCorrect(*lf_0));
    ASSERT_FALSE(fx.iteratorsCorrect(*ld_0));
}

TEST(Sync, assignFilesystemIds_forSingleFile)
{
    Fixture fx{"d"};

    // Level 0
    mt::FsNode d{nullptr, mega::FOLDERNODE, "d"};
    mega::LocalNode& ld = *fx.mSync->localroot;
    static_cast<mega::FileFingerprint&>(ld) = d.getFingerprint();

    // Level 1
    mt::FsNode f_0{&d, mega::FILENODE, "f_0"};
    auto lf_0 = mt::makeLocalNode(*fx.mSync, ld, mega::FILENODE, "f_0", f_0.getFingerprint());

    mt::collectAllFsNodes(fx.mFsNodes, d);

    auto localdebris = LocalPath::fromPath("d/" + mt::gLocalDebris, fx.mFsAccess);
    const auto success = mega::assignFilesystemIds(*fx.mSync, fx.mApp, fx.mFsAccess, fx.mLocalNodes, localdebris);

    ASSERT_TRUE(success);

    ASSERT_EQ(mega::UNDEF, ld.fsid);
    ASSERT_EQ(f_0.getFsId(), lf_0->fsid);

    // assert that the local node map is correct
    constexpr std::size_t fileCount = 1;
    ASSERT_EQ(fileCount, fx.mLocalNodes.size());

    ASSERT_FALSE(fx.iteratorsCorrect(ld));
    ASSERT_TRUE(fx.iteratorsCorrect(*lf_0));
}

TEST(Sync, assignFilesystemIds_whenPathIsNotSyncableThroughApp)
{
    Fixture fx{"d"};
    fx.mApp.addNotSyncablePath(LocalPath::fromPath("d/f_1", fx.mFsAccess));

    // Level 0
    mt::FsNode d{nullptr, mega::FOLDERNODE, "d"};
    mega::LocalNode& ld = *fx.mSync->localroot;
    static_cast<mega::FileFingerprint&>(ld) = d.getFingerprint();

    // Level 1
    mt::FsNode f_0{&d, mega::FILENODE, "f_0"};
    auto lf_0 = mt::makeLocalNode(*fx.mSync, ld, mega::FILENODE, "f_0", f_0.getFingerprint());
    mt::FsNode f_1{&d, mega::FILENODE, "f_1"};

    mt::collectAllFsNodes(fx.mFsNodes, d);

    auto localdebris = LocalPath::fromPath("d/" + mt::gLocalDebris, fx.mFsAccess);
    const auto success = mega::assignFilesystemIds(*fx.mSync, fx.mApp, fx.mFsAccess, fx.mLocalNodes, localdebris);

    ASSERT_TRUE(success);

    ASSERT_EQ(mega::UNDEF, ld.fsid);
    ASSERT_EQ(f_0.getFsId(), lf_0->fsid);

    constexpr std::size_t fileCount = 1;
    ASSERT_EQ(fileCount, fx.mLocalNodes.size());

    ASSERT_FALSE(fx.iteratorsCorrect(ld));
    ASSERT_TRUE(fx.iteratorsCorrect(*lf_0));
}

TEST(Sync, assignFilesystemIds_whenDebrisIsPartOfFiles)
{
    Fixture fx{"d"};

    // Level 0
    mt::FsNode d{nullptr, mega::FOLDERNODE, "d"};
    mega::LocalNode& ld = *fx.mSync->localroot;
    static_cast<mega::FileFingerprint&>(ld) = d.getFingerprint();

    // Level 1
    mt::FsNode f_0{&d, mega::FILENODE, "f_0"};
    auto lf_0 = mt::makeLocalNode(*fx.mSync, ld, mega::FILENODE, "f_0", f_0.getFingerprint());
    mt::FsNode d_1{&d, mega::FOLDERNODE, mt::gLocalDebris};

    // Level 2
    mt::FsNode f_1_0{&d_1, mega::FILENODE, "f_1_0"};

    mt::collectAllFsNodes(fx.mFsNodes, d);

    auto localdebris = LocalPath::fromPath("d/" + mt::gLocalDebris, fx.mFsAccess);
    const auto success = mega::assignFilesystemIds(*fx.mSync, fx.mApp, fx.mFsAccess, fx.mLocalNodes, localdebris);

    ASSERT_TRUE(success);

    // assert that directories have correct fs IDs
    ASSERT_EQ(mega::UNDEF, ld.fsid);

    // assert that all file `LocalNode`s have same fs IDs as the corresponding `FsNode`s
    ASSERT_EQ(f_0.getFsId(), lf_0->fsid);

    // assert that the local node map is correct
    constexpr std::size_t fileCount = 1;
    ASSERT_EQ(fileCount, fx.mLocalNodes.size());

    ASSERT_FALSE(fx.iteratorsCorrect(ld));
    ASSERT_TRUE(fx.iteratorsCorrect(*lf_0));
}

TEST(Sync, assignFilesystemIds_preferredPathMatchAssignsFinalFsId)
{
    Fixture fx{"d"};

    // Level 0
    mt::FsNode d{nullptr, mega::FOLDERNODE, "d"};
    mega::LocalNode& ld = *fx.mSync->localroot;
    static_cast<mega::FileFingerprint&>(ld) = d.getFingerprint();

    // Level 1
    mt::FsNode f_0{&d, mega::FILENODE, "f_0"};
    auto lf_0 = mt::makeLocalNode(*fx.mSync, ld, mega::FILENODE, "f_0", f_0.getFingerprint());
    mt::FsNode d_1{&d, mega::FOLDERNODE, "d_1"};

    // the local node for f_1_0 is still at level 1 but the file moved to level 2 under a new folder (d_1)
    auto lf_1 = mt::makeLocalNode(*fx.mSync, ld, mega::FILENODE, "f_1_0", f_0.getFingerprint());

    // Level 2
    mt::FsNode f_1_0{&d_1, mega::FILENODE, "f_1_0"};
    f_1_0.assignContentFrom(f_0);

    mt::collectAllFsNodes(fx.mFsNodes, d);

    auto localdebris = LocalPath::fromPath("d/" + mt::gLocalDebris, fx.mFsAccess);
    const auto success = mega::assignFilesystemIds(*fx.mSync, fx.mApp, fx.mFsAccess, fx.mLocalNodes, localdebris);

    ASSERT_TRUE(success);

    // assert that directories have correct fs IDs
    ASSERT_EQ(mega::UNDEF, ld.fsid);

    // assert that all file `LocalNode`s have same fs IDs as the corresponding `FsNode`s
    ASSERT_EQ(f_0.getFsId(), lf_0->fsid);
    ASSERT_EQ(f_1_0.getFsId(), lf_1->fsid);

    // assert that the local node map is correct
    constexpr std::size_t fileCount = 2;
    ASSERT_EQ(fileCount, fx.mLocalNodes.size());

    ASSERT_TRUE(fx.iteratorsCorrect(*lf_0));
    ASSERT_TRUE(fx.iteratorsCorrect(*lf_1));
}

TEST(Sync, assignFilesystemIds_whenFolderWasMoved_differentLeafName)
{
    Fixture fx{"d"};

    // Level 0
    mt::FsNode d{nullptr, mega::FOLDERNODE, "d"};
    mega::LocalNode& ld = *fx.mSync->localroot;
    static_cast<mega::FileFingerprint&>(ld) = d.getFingerprint();

    // Level 1
    mt::FsNode d_0_renamed{&d, mega::FOLDERNODE, "d_0_renamed"};
    auto ld_0 = mt::makeLocalNode(*fx.mSync, ld, mega::FOLDERNODE, "d_0", d_0_renamed.getFingerprint());

    // Level 2
    mt::FsNode f_0_0{&d_0_renamed, mega::FILENODE, "f_0_0"};
    auto lf_0_0 = mt::makeLocalNode(*fx.mSync, *ld_0, mega::FILENODE, "f_0_0", f_0_0.getFingerprint());
    mt::FsNode f_0_1{&d_0_renamed, mega::FILENODE, "f_0_1"};
    auto lf_0_1 = mt::makeLocalNode(*fx.mSync, *ld_0, mega::FILENODE, "f_0_1", f_0_1.getFingerprint());

    mt::collectAllFsNodes(fx.mFsNodes, d);

    auto localdebris = LocalPath::fromPath("d/" + mt::gLocalDebris, fx.mFsAccess);
    const auto success = mega::assignFilesystemIds(*fx.mSync, fx.mApp, fx.mFsAccess, fx.mLocalNodes, localdebris);

    ASSERT_TRUE(success);

    // assert that directories have correct fs IDs
    ASSERT_EQ(mega::UNDEF, ld.fsid);
    ASSERT_EQ(mega::UNDEF, ld_0->fsid);

    // assert that all file `LocalNode`s have same fs IDs as the corresponding `FsNode`s
    ASSERT_EQ(f_0_0.getFsId(), lf_0_0->fsid);
    ASSERT_EQ(f_0_1.getFsId(), lf_0_1->fsid);

    // assert that the local node map is correct
    constexpr std::size_t fileCount = 2;
    ASSERT_EQ(fileCount, fx.mLocalNodes.size());

    ASSERT_FALSE(fx.iteratorsCorrect(ld));
    ASSERT_FALSE(fx.iteratorsCorrect(*ld_0));
    ASSERT_TRUE(fx.iteratorsCorrect(*lf_0_0));
    ASSERT_TRUE(fx.iteratorsCorrect(*lf_0_1));
}

TEST(Sync, assignFilesystemIds_whenFolderWasMoved_sameLeafName)
{
    Fixture fx{"d"};

    // Level 0
    mt::FsNode d{nullptr, mega::FOLDERNODE, "d"};
    mega::LocalNode& ld = *fx.mSync->localroot;
    static_cast<mega::FileFingerprint&>(ld) = d.getFingerprint();

    // Level 1
    mt::FsNode d_0_renamed{&d, mega::FOLDERNODE, "d_0_renamed"};
    auto ld_0 = mt::makeLocalNode(*fx.mSync, ld, mega::FOLDERNODE, "d_0", d_0_renamed.getFingerprint());

    // Level 2
    mt::FsNode d_0{&d_0_renamed, mega::FOLDERNODE, "d_0"};

    // Level 3
    mt::FsNode f_0_0{&d_0, mega::FILENODE, "f_0_0"};
    auto lf_0_0 = mt::makeLocalNode(*fx.mSync, *ld_0, mega::FILENODE, "f_0_0", f_0_0.getFingerprint());
    mt::FsNode f_0_1{&d_0, mega::FILENODE, "f_0_1"};
    auto lf_0_1 = mt::makeLocalNode(*fx.mSync, *ld_0, mega::FILENODE, "f_0_1", f_0_1.getFingerprint());

    mt::collectAllFsNodes(fx.mFsNodes, d);

    auto localdebris = LocalPath::fromPath("d/" + mt::gLocalDebris, fx.mFsAccess);
    const auto success = mega::assignFilesystemIds(*fx.mSync, fx.mApp, fx.mFsAccess, fx.mLocalNodes, localdebris);

    ASSERT_TRUE(success);

    // assert that directories have correct fs IDs
    ASSERT_EQ(mega::UNDEF, ld.fsid);
    ASSERT_EQ(d_0.getFsId(), ld_0->fsid);

    // assert that all file `LocalNode`s have same fs IDs as the corresponding `FsNode`s
    ASSERT_EQ(f_0_0.getFsId(), lf_0_0->fsid);
    ASSERT_EQ(f_0_1.getFsId(), lf_0_1->fsid);

    // assert that the local node map is correct
    constexpr std::size_t fileCount = 3;
    ASSERT_EQ(fileCount, fx.mLocalNodes.size());

    ASSERT_FALSE(fx.iteratorsCorrect(ld));
    ASSERT_TRUE(fx.iteratorsCorrect(*ld_0));
    ASSERT_TRUE(fx.iteratorsCorrect(*lf_0_0));
    ASSERT_TRUE(fx.iteratorsCorrect(*lf_0_1));
}

TEST(Sync, assignFilesystemIds_whenFileWasCopied)
{
    Fixture fx{"d"};

    // Level 0
    mt::FsNode d{nullptr, mega::FOLDERNODE, "d"};
    mega::LocalNode& ld = *fx.mSync->localroot;
    static_cast<mega::FileFingerprint&>(ld) = d.getFingerprint();

    // Level 1
    mt::FsNode f_0{&d, mega::FILENODE, "f_0"};
    mt::FsNode d_0{&d, mega::FOLDERNODE, "d_0"};

    auto lf_0 = mt::makeLocalNode(*fx.mSync, ld, mega::FILENODE, "f_0", f_0.getFingerprint());

    // Level 2
    mt::FsNode f_1{&d_0, mega::FILENODE, "f_0"}; // same name as `f_0`
    f_1.assignContentFrom(f_0); // file was copied maintaining mtime

    mt::collectAllFsNodes(fx.mFsNodes, d);

    auto localdebris = LocalPath::fromPath("d/" + mt::gLocalDebris, fx.mFsAccess);
    const auto success = mega::assignFilesystemIds(*fx.mSync, fx.mApp, fx.mFsAccess, fx.mLocalNodes, localdebris);

    ASSERT_TRUE(success);

    // assert that directories have correct fs IDs
    ASSERT_EQ(mega::UNDEF, ld.fsid);

    // assert that all file `LocalNode`s have same fs IDs as the corresponding `FsNode`s
    ASSERT_EQ(f_0.getFsId(), lf_0->fsid);

    // assert that the local node map is correct
    constexpr std::size_t fileCount = 1;
    ASSERT_EQ(fileCount, fx.mLocalNodes.size());

    ASSERT_FALSE(fx.iteratorsCorrect(ld));
    ASSERT_TRUE(fx.iteratorsCorrect(*lf_0));
}

TEST(Sync, assignFilesystemIds_whenFileWasMoved_differentLeafName)
{
    Fixture fx{"d"};

    // Level 0
    mt::FsNode d{nullptr, mega::FOLDERNODE, "d"};
    mega::LocalNode& ld = *fx.mSync->localroot;
    static_cast<mega::FileFingerprint&>(ld) = d.getFingerprint();

    // Level 1
    mt::FsNode f_0{&d, mega::FILENODE, "f_0_renamed"};
    auto lf_0 = mt::makeLocalNode(*fx.mSync, ld, mega::FILENODE, "f_0", f_0.getFingerprint());

    mt::collectAllFsNodes(fx.mFsNodes, d);

    auto localdebris = LocalPath::fromPath("d/" + mt::gLocalDebris, fx.mFsAccess);
    const auto success = mega::assignFilesystemIds(*fx.mSync, fx.mApp, fx.mFsAccess, fx.mLocalNodes, localdebris);

    ASSERT_TRUE(success);

    ASSERT_EQ(mega::UNDEF, ld.fsid);
    ASSERT_EQ(mega::UNDEF, lf_0->fsid);

    // assert that the local node map is correct
    constexpr std::size_t fileCount = 0;
    ASSERT_EQ(fileCount, fx.mLocalNodes.size());

    ASSERT_FALSE(fx.iteratorsCorrect(ld));
    ASSERT_FALSE(fx.iteratorsCorrect(*lf_0));
}

TEST(Sync, assignFilesystemIds_whenFileWasMoved_sameLeafName)
{
    Fixture fx{"d"};

    // Level 0
    mt::FsNode d{nullptr, mega::FOLDERNODE, "d"};
    mega::LocalNode& ld = *fx.mSync->localroot;
    static_cast<mega::FileFingerprint&>(ld) = d.getFingerprint();

    // Level 1
    mt::FsNode d_0{&d, mega::FOLDERNODE, "d_0"};

    // Level 2
    mt::FsNode f_0{&d_0, mega::FILENODE, "f_0"};

    auto lf_0 = mt::makeLocalNode(*fx.mSync, ld, mega::FILENODE, "f_0", f_0.getFingerprint()); // still at level 1

    mt::collectAllFsNodes(fx.mFsNodes, d);

    auto localdebris = LocalPath::fromPath("d/" + mt::gLocalDebris, fx.mFsAccess);
    const auto success = mega::assignFilesystemIds(*fx.mSync, fx.mApp, fx.mFsAccess, fx.mLocalNodes, localdebris);

    ASSERT_TRUE(success);

    ASSERT_EQ(mega::UNDEF, ld.fsid);
    ASSERT_EQ(f_0.getFsId(), lf_0->fsid);

    // assert that the local node map is correct
    constexpr std::size_t fileCount = 1;
    ASSERT_EQ(fileCount, fx.mLocalNodes.size());

    ASSERT_FALSE(fx.iteratorsCorrect(ld));
    ASSERT_TRUE(fx.iteratorsCorrect(*lf_0));
}

TEST(Sync, assignFilesystemIds_emptyFolderStaysUnassigned)
{
    Fixture fx{"d"};

    // Level 0
    mt::FsNode d{nullptr, mega::FOLDERNODE, "d"};
    mega::LocalNode& ld = *fx.mSync->localroot;
    static_cast<mega::FileFingerprint&>(ld) = d.getFingerprint();

    // Level 1
    mt::FsNode d_0{&d, mega::FOLDERNODE, "d_0"};
    auto ld_0 = mt::makeLocalNode(*fx.mSync, ld, mega::FOLDERNODE, "d_0");

    mt::collectAllFsNodes(fx.mFsNodes, d);

    auto localdebris = LocalPath::fromPath("d/" + mt::gLocalDebris, fx.mFsAccess);
    const auto success = mega::assignFilesystemIds(*fx.mSync, fx.mApp, fx.mFsAccess, fx.mLocalNodes, localdebris);

    ASSERT_TRUE(success);

    ASSERT_EQ(mega::UNDEF, ld.fsid);
    ASSERT_EQ(mega::UNDEF, ld_0->fsid);

    // assert that the local node map is correct
    constexpr std::size_t fileCount = 0;
    ASSERT_EQ(fileCount, fx.mLocalNodes.size());

    ASSERT_FALSE(fx.iteratorsCorrect(ld));
    ASSERT_FALSE(fx.iteratorsCorrect(*ld_0));
}

#ifdef NDEBUG
TEST(Sync, assignFilesystemIds_whenRootPathIsNotAFolder_hittingAssert)
{
    Fixture fx{"d"};

    // Level 0
    mt::FsNode d{nullptr, mega::FILENODE, "d"};

    mt::collectAllFsNodes(fx.mFsNodes, d);

    auto localdebris = LocalPath::fromPath("d/" + mt::gLocalDebris, fx.mFsAccess);
    const auto success = mega::assignFilesystemIds(*fx.mSync, fx.mApp, fx.mFsAccess, fx.mLocalNodes, localdebris);

    ASSERT_FALSE(success);
}
#endif

#ifdef NDEBUG
TEST(Sync, assignFilesystemIds_whenFileTypeIsUnexpected_hittingAssert)
{
    Fixture fx{"d"};

    // Level 0
    mt::FsNode d{nullptr, mega::FOLDERNODE, "d"};
    mega::LocalNode& ld = *fx.mSync->localroot;
    static_cast<mega::FileFingerprint&>(ld) = d.getFingerprint();

    // Level 1
    mt::FsNode f_0{&d, mega::TYPE_UNKNOWN, "f_0"};
    auto lf_0 = mt::makeLocalNode(*fx.mSync, ld, mega::FILENODE, "f_0", f_0.getFingerprint());

    mt::collectAllFsNodes(fx.mFsNodes, d);

    auto localdebris = LocalPath::fromPath("d/" + mt::gLocalDebris, fx.mFsAccess);
    const auto success = mega::assignFilesystemIds(*fx.mSync, fx.mApp, fx.mFsAccess, fx.mLocalNodes, localdebris);

    ASSERT_FALSE(success);
}
#endif
*/



namespace mega {
    enum { TYPE_TWOWAY = SyncConfig::TYPE_TWOWAY };
    enum { TYPE_UP = SyncConfig::TYPE_UP };
    enum { TYPE_DOWN = SyncConfig::TYPE_DOWN };
};

/*
TEST(Sync, SyncConfig_defaultOptions)
{
    const mega::SyncConfig config{"foo", "foo", 42, "remote",123};
    ASSERT_TRUE(config.getEnabled());
    ASSERT_EQ("foo", config.getLocalPath());
    ASSERT_EQ(42, config.getRemoteNode());
    ASSERT_EQ(123, config.getLocalFingerprint());
    ASSERT_TRUE(config.getRegExps().empty());
    ASSERT_EQ(mega::TYPE_TWOWAY, config.getType());
    ASSERT_TRUE(config.isUpSync());
    ASSERT_TRUE(config.isDownSync());
    ASSERT_TRUE(config.syncDeletions());
    ASSERT_FALSE(config.forceOverwrite());
    test_SyncConfig_serialization(config);
}

TEST(Sync, SyncConfig_defaultOptions_inactive)
{
    mega::SyncConfig config{"foo", "foo", 42, "remote",123};
    config.setEnabled(false);
    ASSERT_FALSE(config.getEnabled());
    ASSERT_EQ("foo", config.getLocalPath());
    ASSERT_EQ(42, config.getRemoteNode());
    ASSERT_EQ(123, config.getLocalFingerprint());
    ASSERT_TRUE(config.getRegExps().empty());
    ASSERT_EQ(mega::TYPE_TWOWAY, config.getType());
    ASSERT_TRUE(config.isUpSync());
    ASSERT_TRUE(config.isDownSync());
    ASSERT_TRUE(config.syncDeletions());
    ASSERT_FALSE(config.forceOverwrite());
    test_SyncConfig_serialization(config);
}

TEST(Sync, SyncConfig_defaultOptions_butWithRegExps)
{
    const std::vector<std::string> regExps{"aa", "bbb"};
    const mega::SyncConfig config{"foo", "foo", 42, "remote",123, regExps};
    ASSERT_TRUE(config.getEnabled());
    ASSERT_EQ("foo", config.getLocalPath());
    ASSERT_EQ(42, config.getRemoteNode());
    ASSERT_EQ(123, config.getLocalFingerprint());
    ASSERT_EQ(regExps, config.getRegExps());
    ASSERT_EQ(mega::TYPE_TWOWAY, config.getType());
    ASSERT_TRUE(config.isUpSync());
    ASSERT_TRUE(config.isDownSync());
    ASSERT_TRUE(config.syncDeletions());
    ASSERT_FALSE(config.forceOverwrite());
    test_SyncConfig_serialization(config);
}

TEST(Sync, SyncConfig_upSync_syncDelFalse_overwriteFalse)
{
    const std::vector<std::string> regExps{"aa", "bbb"};
    const mega::SyncConfig config{"foo", "foo", 42, "remote",123, regExps, true, mega::SyncConfig::TYPE_UP};
    ASSERT_TRUE(config.getEnabled());
    ASSERT_EQ("foo", config.getLocalPath());
    ASSERT_EQ(42, config.getRemoteNode());
    ASSERT_EQ(123, config.getLocalFingerprint());
    ASSERT_EQ(regExps, config.getRegExps());
    ASSERT_EQ(mega::TYPE_UP, config.getType());
    ASSERT_TRUE(config.isUpSync());
    ASSERT_FALSE(config.isDownSync());
    ASSERT_FALSE(config.syncDeletions());
    ASSERT_FALSE(config.forceOverwrite());
    test_SyncConfig_serialization(config);
}

TEST(Sync, SyncConfig_upSync_syncDelTrue_overwriteTrue)
{
    const std::vector<std::string> regExps{"aa", "bbb"};
    const mega::SyncConfig config{"foo", "foo", 42, "remote",123, regExps, true, mega::SyncConfig::TYPE_UP, true, true};
    ASSERT_TRUE(config.getEnabled());
    ASSERT_EQ("foo", config.getLocalPath());
    ASSERT_EQ(42, config.getRemoteNode());
    ASSERT_EQ(123, config.getLocalFingerprint());
    ASSERT_EQ(regExps, config.getRegExps());
    ASSERT_EQ(mega::TYPE_UP, config.getType());
    ASSERT_TRUE(config.isUpSync());
    ASSERT_FALSE(config.isDownSync());
    ASSERT_TRUE(config.syncDeletions());
    ASSERT_TRUE(config.forceOverwrite());
    test_SyncConfig_serialization(config);
}

TEST(Sync, SyncConfig_downSync_syncDelFalse_overwriteFalse)
{
    const std::vector<std::string> regExps{"aa", "bbb"};
    const mega::SyncConfig config{"foo", "foo", 42, "remote",123, regExps, true, mega::SyncConfig::TYPE_DOWN};
    ASSERT_TRUE(config.getEnabled());
    ASSERT_EQ("foo", config.getLocalPath());
    ASSERT_EQ(42, config.getRemoteNode());
    ASSERT_EQ(123, config.getLocalFingerprint());
    ASSERT_EQ(regExps, config.getRegExps());
    ASSERT_EQ(mega::TYPE_DOWN, config.getType());
    ASSERT_FALSE(config.isUpSync());
    ASSERT_TRUE(config.isDownSync());
    ASSERT_FALSE(config.syncDeletions());
    ASSERT_FALSE(config.forceOverwrite());
    test_SyncConfig_serialization(config);
}

TEST(Sync, SyncConfig_downSync_syncDelTrue_overwriteTrue)
{
    const std::vector<std::string> regExps{"aa", "bbb"};
    const mega::SyncConfig config{"foo", "foo", 42, "remote",123, regExps, true, mega::SyncConfig::TYPE_DOWN, true, true};
    ASSERT_TRUE(config.getEnabled());
    ASSERT_EQ("foo", config.getLocalPath());
    ASSERT_EQ(42, config.getRemoteNode());
    ASSERT_EQ(123, config.getLocalFingerprint());
    ASSERT_EQ(regExps, config.getRegExps());
    ASSERT_EQ(mega::SyncConfig::TYPE_DOWN, config.getType());
    ASSERT_FALSE(config.isUpSync());
    ASSERT_TRUE(config.isDownSync());
    ASSERT_TRUE(config.syncDeletions());
    ASSERT_TRUE(config.forceOverwrite());
    test_SyncConfig_serialization(config);
}
*/

#if 0
namespace
{

void test_SyncConfigBag(mega::SyncConfigBag& bag)
{
    ASSERT_TRUE(bag.all().empty());
    mega::SyncConfig config1{"foo", "foo", 41, "remote", 122, {}, true, mega::SyncConfig::Type::TYPE_TWOWAY, false, true, mega::LOCAL_FINGERPRINT_MISMATCH};
    config1.setBackupId(12345);
    bag.insert(config1);
    mega::SyncConfig config2{"bar", "bar", 42, "remote", 123, {}, false, mega::SyncConfig::Type::TYPE_UP, true, false, mega::NO_SYNC_ERROR};
    config2.setBackupId(123456);
    bag.insert(config2);
    std::vector<mega::SyncConfig> expConfigs1{config1, config2};
    //ASSERT_EQ(expConfigs1, bag.all());
    bag.removeByBackupId(12345);
    std::vector<mega::SyncConfig> expConfigs2{config2};
    //ASSERT_EQ(expConfigs2, bag.all());
    mega::SyncConfig config3{"bar2", "bar2", 43, "remote", 124};
    config3.setBackupId(1234567);
    bag.insert(config3); // update
    std::vector<mega::SyncConfig> expConfigs3{config3};
    //ASSERT_EQ(expConfigs3, bag.all());
    bag.insert(config1);
    bag.insert(config2);
    //ASSERT_EQ(expConfigs1, bag.all());
    bag.clear();
    ASSERT_TRUE(bag.all().empty());
}

class MockDbTable : public mt::DefaultedDbTable
{
public:
    using mt::DefaultedDbTable::DefaultedDbTable;
    void rewind() override
    {
        mIndex = 0;
    }
    bool next(uint32_t* id, std::string* data) override
    {
        if (mIndex >= mData->size())
        {
            return false;
        }
        *id = (*mData)[mIndex].first;
        *data = (*mData)[mIndex].second;
        ++mIndex;
        return true;
    }
    bool put(uint32_t id, char* data, unsigned size) override
    {
        del(id);
        mData->emplace_back(id, std::string{data, size});
        return true;
    }
    bool del(uint32_t id) override
    {
        mData->erase(std::remove_if(mData->begin(), mData->end(),
                                   [id](const std::pair<uint32_t, std::string>& p)
                                   {
                                       return p.first == id;
                                   }),
                     mData->end());
        return true;
    }
    void truncate() override
    {
        mData->clear();
    }

    bool inTransaction() const override
    {
        return true;
    }

    std::vector<std::pair<uint32_t, std::string>>* mData = nullptr;

private:
    size_t mIndex = 0;
};

class MockDbAccess : public mega::DbAccess
{
public:
    MockDbAccess(std::vector<std::pair<uint32_t, std::string>>& data)
        : mData{data}
    {}
    mega::DbTable* open(mega::PrnGen &rng, mega::FileSystemAccess&, const std::string&, const int flags) override
    {
        auto table = new MockDbTable{rng, (flags & mega::DB_OPEN_FLAG_TRANSACTED) > 0};
        table->mData = &mData;
        return table;
    }

    bool probe(mega::FileSystemAccess&, const string&) const override
    {
        return true;
    }

    const mega::LocalPath &rootPath() const override
    {
        static mega::LocalPath const dummy;

        return dummy;
    }

private:
    std::vector<std::pair<uint32_t, std::string>>& mData;
};

}

TEST(Sync, SyncConfigBag)
{
    std::vector<std::pair<uint32_t, std::string>> mData;
    MockDbAccess dbaccess{mData};
    mt::DefaultedFileSystemAccess fsaccess;
    mega::PrnGen rng;
    mega::SyncConfigBag bag{dbaccess, fsaccess, rng, "some_id"};
    test_SyncConfigBag(bag);
}

TEST(Sync, SyncConfigBag_withPreviousState)
{
    std::vector<std::pair<uint32_t, std::string>> mData;
    MockDbAccess dbaccess{mData};
    mt::DefaultedFileSystemAccess fsaccess;
    mega::PrnGen rng;

    mega::SyncConfigBag bag1{dbaccess, fsaccess, rng, "some_id"};
    mega::SyncConfig config1{"foo", "foo", 41, "remote", 122, {}, true, mega::SyncConfig::Type::TYPE_TWOWAY, false, true, mega::LOCAL_FINGERPRINT_MISMATCH};
    config1.setBackupId(12345);
    bag1.insert(config1);
    ASSERT_EQ(1u, mData.size());
    mega::SyncConfig config2{"bar", "bar", 42, "remote", 123, {}, false, mega::SyncConfig::Type::TYPE_UP, true, false, mega::NO_SYNC_ERROR};
    config2.setBackupId(123456);
    bag1.insert(config2);
    ASSERT_EQ(2u, mData.size());
    mega::SyncConfig config3{"bar2", "bar2", 43, "remote", 124, {}, false, mega::SyncConfig::Type::TYPE_UP, true, false, mega::NO_SYNC_ERROR};
    config3.setBackupId(1234567);
    bag1.insert(config3);
    ASSERT_EQ(3u, mData.size());
    bag1.insert(config3); // update
    ASSERT_EQ(3u, mData.size());
    bag1.removeByBackupId(1234567);
    ASSERT_EQ(2u, mData.size());

    const mega::SyncConfigBag bag2{dbaccess, fsaccess, rng, "some_id"};
    const std::vector<mega::SyncConfig> expConfigs{config1, config2};
    //ASSERT_EQ(expConfigs, bag2.all());
}
#endif


namespace JSONSyncConfigTests
{

using namespace mega;
using namespace testing;

class Directory
{
public:
    Directory(FSACCESS_CLASS& fsAccess, const LocalPath& path)
      : mFSAccess(fsAccess)
      , mPath(path)
    {
        mFSAccess.mkdirlocal(mPath, false);
    }

    ~Directory()
    {
        mFSAccess.emptydirlocal(mPath);
        mFSAccess.rmdirlocal(mPath);
    }

    MEGA_DISABLE_COPY_MOVE(Directory);

    operator const LocalPath&() const
    {
        return mPath;
    }

    const LocalPath& path() const
    {
        return mPath;
    }

private:
    FSACCESS_CLASS& mFSAccess;
    LocalPath mPath;
}; // Directory

// Temporary shims so that we can easily switch to using
// NiceMock / FakeStrictMock when GMock/GTest is upgraded on Jenkins.
#if 1

template<typename MockClass>
class FakeNiceMock
    : public MockClass
{
public:
        using MockClass::MockClass;
}; // FakeNiceMock<T>

template<typename MockClass>
class FakeStrictMock
    : public MockClass
{
public:
        using MockClass::MockClass;
}; // FakeStrictMock<T>

#else

template<typename T>
using FakeNiceMock = NiceMock<T>;

template<typename T>
using FakeStrictMock = StrictMock<T>;

#endif

class Utilities
{
public:
    static string randomBase64(const size_t n = 16)
    {
        return Base64::btoa(randomBytes(n));
    }

    static string randomBytes(const size_t n)
    {
        string result(n, '0');

        mRNG.genblock(reinterpret_cast<byte*>(&result[0]), n);

        return result;
    }

    static bool randomFile(LocalPath path, const size_t n = 64)
    {
        auto fileAccess = mFSAccess.newfileaccess(false);

        if (!fileAccess->fopen(path, false, true))
        {
            return false;
        }

        if (fileAccess->size > 0)
        {
            if (!fileAccess->ftruncate())
            {
                return false;
            }
        }

        const string data = randomBytes(n);
        const byte* bytes = reinterpret_cast<const byte*>(&data[0]);

        return fileAccess->fwrite(bytes, static_cast<unsigned>(n), 0x0);
    }

    static LocalPath randomPath(const size_t n = 16)
    {
        return LocalPath::fromPath(randomBase64(n), mFSAccess);
    }

    static LocalPath separator()
    {
#ifdef _WIN32
        return LocalPath::fromPath("\\", mFSAccess);
#else // _WIN32
        return LocalPath::fromPath("/", mFSAccess);
#endif // ! _WIN32
    }

private:
    static FSACCESS_CLASS mFSAccess;
    static PrnGen mRNG;
}; // Utilities

FSACCESS_CLASS Utilities::mFSAccess;
PrnGen Utilities::mRNG;

class JSONSyncConfigTest
  : public Test
{
public:
    class IOContext
      : public JSONSyncConfigIOContext
    {
    public:
        IOContext(SymmCipher& cipher,
                  FileSystemAccess& fsAccess,
                  const string& key,
                  const string& name,
                  PrnGen& rng)
          : JSONSyncConfigIOContext(cipher,
                                   fsAccess,
                                   key,
                                   name,
                                   rng)
        {
            // Perform real behavior by default.
            ON_CALL(*this, getSlotsInOrder(_, _))
              .WillByDefault(Invoke(this, &IOContext::getSlotsInOrderConcrete));

            ON_CALL(*this, read(_, _, _))
              .WillByDefault(Invoke(this, &IOContext::readConcrete));

            ON_CALL(*this, write(_, _, _))
              .WillByDefault(Invoke(this, &IOContext::writeConcrete));
        }

        MOCK_METHOD2(getSlotsInOrder, error(const LocalPath&, vector<unsigned int>&));

        MOCK_METHOD3(read, error(const LocalPath&, string&, const unsigned int));

        MOCK_METHOD3(write, error(const LocalPath&, const string&, const unsigned int));

    private:
        // Delegate to real behavior.
        error getSlotsInOrderConcrete(const LocalPath& drivePath,
                                      vector<unsigned int>& slots)
        {
            return JSONSyncConfigIOContext::getSlotsInOrder(drivePath, slots);
        }

        error readConcrete(const LocalPath& drivePath,
                           string& data,
                           const unsigned int slot)
        {
            return JSONSyncConfigIOContext::read(drivePath, data, slot);
        }

        error writeConcrete(const LocalPath& drivePath,
                            const string& data,
                            const unsigned int slot)
        {
            return JSONSyncConfigIOContext::write(drivePath, data, slot);
        }
    }; // IOContext

    JSONSyncConfigTest()
      : Test()
      , mCipher(SymmCipher::zeroiv)
      , mFSAccess()
      , mRNG()
      , mConfigKey(Utilities::randomBase64(32))
      , mConfigName(Utilities::randomBase64(16))
      , mConfigPrefix("megaclient_syncconfig_")
      , mIOContext(mCipher,
                   mFSAccess,
                   mConfigKey,
                   mConfigName,
                   mRNG)
    {
    }

    FSACCESS_CLASS& fsAccess()
    {
        return mFSAccess;
    }

    IOContext& ioContext()
    {
        return mIOContext;
    }

protected:
    SymmCipher mCipher;
    FSACCESS_CLASS mFSAccess;
    PrnGen mRNG;
    const string mConfigKey;
    const string mConfigName;
    const string mConfigPrefix;
    FakeNiceMock<IOContext> mIOContext;
}; // JSONSyncConfigTest

class JSONSyncConfigIOContextTest
  : public JSONSyncConfigTest
{
public:
    JSONSyncConfigIOContextTest()
      : JSONSyncConfigTest()
    {
    }

    string configName() const
    {
        return mConfigPrefix + mConfigName;
    }

    const string& configPrefix() const
    {
        return mConfigPrefix;
    }
}; // JSONSyncConfigIOContextTest

TEST_F(JSONSyncConfigIOContextTest, GetBadPath)
{
    vector<unsigned int> slots;

    // Generate a bogus path.
    const auto drivePath = Utilities::randomPath();

    // Try to read slots from an invalid path.
    EXPECT_NE(ioContext().getSlotsInOrder(drivePath, slots), API_OK);

    // Slots should be empty.
    EXPECT_TRUE(slots.empty());
}

TEST_F(JSONSyncConfigIOContextTest, GetNoSlots)
{
    // Make sure the drive path exists.
    Directory drive(fsAccess(), Utilities::randomPath());

    // Generate some malformed slots for this user.
    {
        LocalPath configPath = drive;

        // This file will be ignored as it has no slot suffix.
        configPath.appendWithSeparator(
          LocalPath::fromPath(configName(), fsAccess()), false);
        EXPECT_TRUE(Utilities::randomFile(configPath));

        // This file will be ignored as it has a malformed slot suffix.
        configPath.append(LocalPath::fromPath(".", fsAccess()));
        EXPECT_TRUE(Utilities::randomFile(configPath));

        // This file will be ignored as it has an invalid slot suffix.
        configPath.append(LocalPath::fromPath("Q", fsAccess()));
        EXPECT_TRUE(Utilities::randomFile(configPath));
    }

    // Generate a slot for a different user.
    {
        LocalPath configPath = drive;

        configPath.appendWithSeparator(
          LocalPath::fromPath(configPrefix(), fsAccess()), false);
        configPath.append(Utilities::randomPath());
        configPath.append(LocalPath::fromPath(".0", fsAccess()));

        EXPECT_TRUE(Utilities::randomFile(configPath));
    }

    vector<unsigned int> slots;

    // Try and get a list of slots.
    EXPECT_EQ(ioContext().getSlotsInOrder(drive.path(), slots), API_OK);

    // Slots should be empty.
    EXPECT_TRUE(slots.empty());
}

TEST_F(JSONSyncConfigIOContextTest, GetSlotsOrderedByModificationTime)
{
    const size_t NUM_SLOTS = 3;

    // Make sure drive path exists.
    Directory drive(fsAccess(), Utilities::randomPath());

    // Generate some slots for this user.
    {
        LocalPath configPath = drive;

        // Generate suitable config path prefix.
        configPath.appendWithSeparator(
          LocalPath::fromPath(configName(), fsAccess()), false);

        for (size_t i = 0; i < NUM_SLOTS; ++i)
        {
            using std::to_string;

            ScopedLengthRestore restorer(configPath);

            // Generate suffix.
            LocalPath suffixPath =
              LocalPath::fromPath("." + to_string(i), fsAccess());

            // Complete config path.
            configPath.append(suffixPath);

            // Populate the file.
            EXPECT_TRUE(Utilities::randomFile(configPath));

            // Set the modification time.
            EXPECT_TRUE(fsAccess().setmtimelocal(configPath, i * 1000));
        }
    }

    vector<unsigned int> slots;

    // Get the slots.
    EXPECT_EQ(ioContext().getSlotsInOrder(drive.path(), slots), API_OK);

    // Did we retrieve the correct number of slots?
    ASSERT_EQ(slots.size(), NUM_SLOTS);

    // Are the slots ordered by descending modification time?
    {
        vector<unsigned int> expected(NUM_SLOTS, 0);

        iota(begin(expected), end(expected), 0);

        EXPECT_TRUE(equal(begin(expected), end(expected), rbegin(slots)));
    }
}

TEST_F(JSONSyncConfigIOContextTest, GetSlotsOrderedBySlotSuffix)
{
    const size_t NUM_SLOTS = 3;

    // Make sure drive path exists.
    Directory drive(fsAccess(), Utilities::randomPath());

    // Generate some slots for this user.
    {
        LocalPath configPath = drive;

        // Generate suitable config path prefix.
        configPath.appendWithSeparator(
          LocalPath::fromPath(configName(), fsAccess()), false);

        for (size_t i = 0; i < NUM_SLOTS; ++i)
        {
            using std::to_string;

            ScopedLengthRestore restorer(configPath);

            // Generate suffix.
            LocalPath suffixPath =
              LocalPath::fromPath("." + to_string(i), fsAccess());

            // Complete config path.
            configPath.append(suffixPath);

            // Populate the file.
            EXPECT_TRUE(Utilities::randomFile(configPath));

            // Set the modification time.
            EXPECT_TRUE(fsAccess().setmtimelocal(configPath, 0));
        }
    }

    vector<unsigned int> slots;

    // Get the slots.
    EXPECT_EQ(ioContext().getSlotsInOrder(drive.path(), slots), API_OK);

    // Did we retrieve the correct number of slots?
    EXPECT_EQ(slots.size(), NUM_SLOTS);

    // Are the slots ordered by descending slot number when their
    // modification time is the same?
    {
        vector<unsigned int> expected(NUM_SLOTS, 0);

        iota(begin(expected), end(expected), 0);

        EXPECT_TRUE(equal(begin(expected), end(expected), rbegin(slots)));
    }
}

TEST_F(JSONSyncConfigIOContextTest, Read)
{
    // Make sure the drive path exists.
    Directory drive(fsAccess(), Utilities::randomPath());

    // Try writing some data out and reading it back again.
    {
        string read;
        string written = Utilities::randomBytes(64);

        EXPECT_EQ(ioContext().write(drive.path(), written, 0), API_OK);
        EXPECT_EQ(ioContext().read(drive.path(), read, 0), API_OK);
        EXPECT_EQ(read, written);
    }

    // Try a different slot to make sure it has an effect.
    {
        string read;
        string written = Utilities::randomBytes(64);

        EXPECT_EQ(ioContext().read(drive.path(), read, 1), API_EREAD);
        EXPECT_TRUE(read.empty());

        EXPECT_EQ(ioContext().write(drive.path(), written, 1), API_OK);
        EXPECT_EQ(ioContext().read(drive.path(), read, 1), API_OK);
        EXPECT_EQ(read, written);
    }
}

TEST_F(JSONSyncConfigIOContextTest, ReadBadData)
{
    string data;

    // Make sure the drive path exists.
    Directory drive(fsAccess(), Utilities::randomPath());

    // Generate slot path.
    LocalPath slotPath = drive;

    slotPath.appendWithSeparator(
      LocalPath::fromPath(configName(), fsAccess()), false);

    slotPath.append(LocalPath::fromPath(".0", fsAccess()));

    // Try loading a file that's too short to be valid.
    EXPECT_TRUE(Utilities::randomFile(slotPath, 1));
    EXPECT_EQ(ioContext().read(drive.path(), data, 0), API_EREAD);
    EXPECT_TRUE(data.empty());

    // Try loading a file composed entirely of junk.
    EXPECT_TRUE(Utilities::randomFile(slotPath, 128));
    EXPECT_EQ(ioContext().read(drive.path(), data, 0), API_EREAD);
    EXPECT_TRUE(data.empty());
}

TEST_F(JSONSyncConfigIOContextTest, ReadBadPath)
{
    const LocalPath drivePath = Utilities::randomPath();
    string data;

    // Try and read data from an insane path.
    EXPECT_EQ(ioContext().read(drivePath, data, 0), API_EREAD);
    EXPECT_TRUE(data.empty());
}

TEST_F(JSONSyncConfigIOContextTest, Serialize)
{
    JSONSyncConfigMap read;
    JSONSyncConfigMap written;
    JSONWriter writer;

    // Populate the database with two configs.
    {
        SyncConfig config;

        config.mBackupId = 1;
        config.mEnabled = false;
        config.mError = NO_SYNC_ERROR;
        config.mLocalFingerprint = 1;
        config.mLocalPath = Utilities::randomPath();
        config.mName = Utilities::randomBase64();
        config.mOrigninalPathOfRemoteRootNode = Utilities::randomBase64();
        config.mRemoteNode = UNDEF;
        config.mWarning = NO_SYNC_WARNING;
        config.mSyncType = SyncConfig::TYPE_TWOWAY;

        written.emplace(config.mBackupId, config);

        config.mBackupId = 2;
        config.mEnabled = true;
        config.mError = UNKNOWN_ERROR;
        config.mLocalFingerprint = 2;
        config.mLocalPath = Utilities::randomPath();
        config.mName = Utilities::randomBase64();
        config.mOrigninalPathOfRemoteRootNode = Utilities::randomBase64();
        config.mRegExps = {"a", "b"};
        config.mRemoteNode = 3;
        config.mWarning = LOCAL_IS_FAT;
        config.mSyncType = SyncConfig::TYPE_BACKUP;

        written.emplace(config.mBackupId, config);
    }

    // Serialize the database.
    ioContext().serialize(written, writer);
    EXPECT_FALSE(writer.getstring().empty());

    // Deserialize the database.
    {
        JSON reader(writer.getstring());
        EXPECT_TRUE(ioContext().deserialize(read, reader));
    }

    // Are the databases identical?
    EXPECT_EQ(read, written);
}

TEST_F(JSONSyncConfigIOContextTest, SerializeEmpty)
{
    JSONWriter writer;

    // Serialize an empty database.
    {
        // Does serializing an empty database yield an empty array?
        ioContext().serialize(JSONSyncConfigMap(), writer);
        EXPECT_EQ(writer.getstring(), "[]");
    }

    // Deserialize the empty database.
    {
        JSONSyncConfigMap configs;
        JSON reader(writer.getstring());

        // Can we deserialize an empty database?
        EXPECT_TRUE(ioContext().deserialize(configs, reader));
        EXPECT_TRUE(configs.empty());
    }
}

TEST_F(JSONSyncConfigIOContextTest, WriteBadPath)
{
    const LocalPath drivePath = Utilities::randomPath();
    const string data = Utilities::randomBytes(64);

    auto dbPath = drivePath;
    dbPath.appendWithSeparator(Utilities::randomPath(), false);

    // Try and write data to an insane path.
    EXPECT_NE(ioContext().write(dbPath, data, 0), API_OK);
}

class JSONSyncConfigDBTest
  : public JSONSyncConfigTest
{
public:
    class Observer
      : public JSONSyncConfigDBObserver
    {
    public:
        // Convenience.
        using Config = SyncConfig;
        using DB = JSONSyncConfigDB;

        MOCK_METHOD2(onAdd, void(DB&, const Config&));

        MOCK_METHOD3(onChange, void(DB&, const Config&, const Config&));

        MOCK_METHOD1(onDirty, void(DB&));

        MOCK_METHOD2(onRemove, void(DB&, const Config&));
    }; // Observer

    JSONSyncConfigDBTest()
      : JSONSyncConfigTest()
      , mDBPath(Utilities::randomPath())
      , mDrivePath(mDBPath)
      , mObserver()
    {
    }

    const LocalPath& dbPath() const
    {
        return mDBPath;
    }

    const LocalPath& drivePath() const
    {
        return mDrivePath;
    }

    Observer& observer()
    {
        return mObserver;
    }

private:
    LocalPath mDBPath;
    const LocalPath mDrivePath;
    FakeNiceMock<Observer> mObserver;
}; // JSONSyncConfigDBTest

TEST_F(JSONSyncConfigDBTest, AddWithTarget)
{
    // Create config DB.
    JSONSyncConfigDB configDB(dbPath(), drivePath(), observer());

    // Create and populate config.
    SyncConfig config;

    config.mExternalDrivePath = drivePath();
    config.mLocalPath = LocalPath();
    config.mEnabled = true;
    config.mBackupId = 0;
    config.mRemoteNode = 1;

    // Database should tell the observer that a new config has been added.
    Expectation onAdd =
      EXPECT_CALL(observer(),
                  onAdd(Ref(configDB), Eq(config)))
        .Times(1);

    // Database should tell the observer it needs to be written.
    EXPECT_CALL(observer(),
                onDirty(Ref(configDB)))
      .After(onAdd);

    // Add config to database.
    const auto* c = configDB.add(config);
    ASSERT_NE(c, nullptr);
    EXPECT_EQ(*c, config);

    // Has a config been added?
    EXPECT_EQ(configDB.configs().size(), 1);

    // Can we retrieve the config by tag?
    EXPECT_EQ(configDB.getByBackupId(config.mBackupId), c);

    // Can we retrieve the config by target handle?
    EXPECT_EQ(configDB.getByRootHandle(config.mRemoteNode), c);
}

TEST_F(JSONSyncConfigDBTest, AddWithoutTarget)
{
    // Create config DB.
    JSONSyncConfigDB configDB(dbPath(), drivePath(), observer());

    // Create and populate config.
    SyncConfig config;

    config.mExternalDrivePath = drivePath();
    config.mLocalPath = LocalPath();
    config.mEnabled = true;
    config.mBackupId = 0;
    config.mRemoteNode = UNDEF;

    // Database should tell the observer that a new config has been added.
    Expectation onAdd =
      EXPECT_CALL(observer(),
                  onAdd(Ref(configDB), Eq(config)))
        .Times(1);

    // Database should tell the observer it needs to be written.
    EXPECT_CALL(observer(),
                onDirty(Ref(configDB)))
      .Times(1)
      .After(onAdd);

    // Add config to database.
    const auto* c = configDB.add(config);
    ASSERT_NE(c, nullptr);
    EXPECT_EQ(*c, config);

    // Has a config been added?
    EXPECT_EQ(configDB.configs().size(), 1);

    // Can we retrieve the config by tag?
    EXPECT_EQ(configDB.getByBackupId(config.mBackupId), c);

    // No mapping should ever be created for an UNDEF handle.
    EXPECT_EQ(configDB.getByRootHandle(UNDEF), nullptr);
}


TEST_F(JSONSyncConfigDBTest, Clear)
{
    JSONSyncConfigDB configDB(dbPath(), drivePath(), observer());

    // Add a couple configurations.
    SyncConfig configA;
    SyncConfig configB;

    configA.mExternalDrivePath = drivePath();
    configA.mLocalPath = Utilities::randomPath();
    configA.mBackupId = 0;
    configA.mRemoteNode = 1;

    configB.mExternalDrivePath = drivePath();
    configB.mLocalPath = Utilities::randomPath();
    configB.mBackupId = 2;
    configB.mRemoteNode = 3;

    EXPECT_NE(configDB.add(configA), nullptr);
    EXPECT_NE(configDB.add(configB), nullptr);

    // Verify configs have been added.
    EXPECT_EQ(configDB.configs().size(), 2);

    // Observer should be notified for each config cleared.
    Expectation onRemoveA =
      EXPECT_CALL(observer(),
                  onRemove(Ref(configDB), Eq(configA)))
        .Times(1);

    Expectation onRemoveB =
      EXPECT_CALL(observer(),
                  onRemove(Ref(configDB), Eq(configB)))
        .Times(1)
        .After(onRemoveA);

    // Observer should be notified that the DB needs writing.
    EXPECT_CALL(observer(),
                onDirty(Ref(configDB)))
      .Times(1)
      .After(onRemoveB);

    // Clear the database.
    configDB.clear();

    // Database shouldn't contain any configs.
    EXPECT_TRUE(configDB.configs().empty());

    // No mappings should remain.
    EXPECT_EQ(configDB.getByBackupId(configA.mBackupId), nullptr);
    EXPECT_EQ(configDB.getByBackupId(configB.mBackupId), nullptr);
    EXPECT_EQ(configDB.getByRootHandle(configA.mRemoteNode), nullptr);
    EXPECT_EQ(configDB.getByRootHandle(configB.mRemoteNode), nullptr);
}

TEST_F(JSONSyncConfigDBTest, ClearEmpty)
{
    JSONSyncConfigDB configDB(dbPath(), drivePath(), observer());

    // Clearing an empty database should not trigger any notifications.
    EXPECT_CALL(observer(), onDirty(_)).Times(0);
    EXPECT_CALL(observer(), onRemove(_, _)).Times(0);

    // Clear the database.
    configDB.clear();
}

TEST_F(JSONSyncConfigDBTest, Destruct)
{
    // Nested scope so we can test destruction.
    {
        JSONSyncConfigDB configDB(dbPath(), drivePath(), observer());

        // Create config.
        SyncConfig config;

        config.mExternalDrivePath = drivePath();
        config.mLocalPath = Utilities::randomPath();
        config.mBackupId = 1;
        config.mRemoteNode = 2;

        // Add config.
        EXPECT_NE(configDB.add(config), nullptr);

        // Observer should be told about each removed config.
        EXPECT_CALL(observer(),
                    onRemove(Ref(configDB), Eq(config)))
          .Times(1);

        // Destructor does not dirty the database.
        EXPECT_CALL(observer(),
                    onDirty(Ref(configDB)))
          .Times(0);
    }
}

TEST_F(JSONSyncConfigDBTest, DrivePath)
{
    JSONSyncConfigDB configDB(dbPath(), drivePath(), observer());

    EXPECT_EQ(configDB.drivePath(), drivePath());
}

TEST_F(JSONSyncConfigDBTest, DestructEmpty)
{
    // Nested scope so we can test destruction.
    {
        JSONSyncConfigDB configDB(dbPath(), drivePath(), observer());

        // An empty database should not generate any notifications.
        EXPECT_CALL(observer(), onDirty(_)).Times(0);
        EXPECT_CALL(observer(), onRemove(_, _)).Times(0);
    }
}

TEST_F(JSONSyncConfigDBTest, Read)
{
    JSONSyncConfigDB configDB(dbPath(), drivePath(), observer());

    // Add a configuration to be written to disk.
    SyncConfig config;

    config.mExternalDrivePath = drivePath();
    config.mLocalPath = Utilities::randomPath();
    config.mBackupId = 1;
    config.mRemoteNode = 2;

    // Add the config to the database.
    EXPECT_NE(configDB.add(config), nullptr);

    // Write the config to disk.
    string json;

    // Capture the JSON and signal write success.
    EXPECT_CALL(ioContext(),
                write(Eq(dbPath()), _, Eq(0u)))
      .WillOnce(DoAll(SaveArg<1>(&json),
                      Return(API_OK)));

    // Write the database to disk.
    EXPECT_EQ(configDB.write(ioContext()), API_OK);

    // Clear the database.
    configDB.clear();

    // Read the configuration back.
    static const vector<unsigned int> slots = {0};

    // Return a single slot for reading.
    Expectation get =
      EXPECT_CALL(ioContext(),
                  getSlotsInOrder(Eq(dbPath()), _))
        .WillOnce(DoAll(SetArgReferee<1>(slots),
                        Return(API_OK)));

    // Read should return the captured JSON.
    Expectation read =
      EXPECT_CALL(ioContext(),
                  read(Eq(dbPath()), _, Eq(0u)))
        .After(get)
        .WillOnce(DoAll(SetArgReferee<1>(json),
                        Return(API_OK)));

    // Observer should be notified when a configuration is loaded.
    EXPECT_CALL(observer(),
                onAdd(Ref(configDB), Eq(config)))
      .Times(1)
      .After(read);

    // Loading should not trigger any dirty notifications.
    EXPECT_CALL(observer(), onDirty(Ref(configDB))).Times(0);

    // Read should succeed.
    EXPECT_EQ(configDB.read(ioContext()), API_OK);

    // Can we retrieve the loaded config by tag?
    const auto* c = configDB.getByBackupId(config.mBackupId);
    ASSERT_NE(c, nullptr);
    EXPECT_EQ(*c, config);

    // Can we retrieve the loaded config by target handle?
    EXPECT_EQ(configDB.getByRootHandle(config.mRemoteNode), c);
}

TEST_F(JSONSyncConfigDBTest, ReadBadDecrypt)
{
    static const vector<unsigned int> slots = {1};

    JSONSyncConfigDB configDB(dbPath(), drivePath(), observer());

    // Return a single slot for reading.
    Expectation get =
      EXPECT_CALL(ioContext(),
                  getSlotsInOrder(Eq(dbPath()), _))
        .WillOnce(DoAll(SetArgReferee<1>(slots),
                        Return(API_OK)));

    // Force the slot read to fail.
    EXPECT_CALL(ioContext(),
                read(Eq(dbPath()), _, Eq(slots.front())))
      .After(get)
      .WillOnce(Return(API_EREAD));

    // Read should fail if we can't read from the only available slot.
    EXPECT_EQ(configDB.read(ioContext()), API_EREAD);
}

TEST_F(JSONSyncConfigDBTest, ReadEmptyClearsDatabase)
{
    JSONSyncConfigDB configDB(dbPath(), drivePath(), observer());

    // Add a config to the database.
    SyncConfig config;

    config.mExternalDrivePath = drivePath();
    config.mBackupId = 1;
    config.mRemoteNode = 2;

    EXPECT_NE(configDB.add(config), nullptr);

    // Return a single slot for reading.
    static const vector<unsigned int> slots = {0};

    Expectation get =
      EXPECT_CALL(ioContext(),
                  getSlotsInOrder(Eq(dbPath()), _))
        .WillOnce(DoAll(SetArgReferee<1>(slots),
                        Return(API_OK)));

    // Read yields an empty database.
    Expectation read =
      EXPECT_CALL(ioContext(),
                  read(Eq(dbPath()), _, Eq(0u)))
        .After(get)
        .WillOnce(DoAll(SetArgReferee<1>("[]"),
                        Return(API_OK)));

    // Observer should be notified that the config has been removed.
    EXPECT_CALL(observer(),
                onRemove(Ref(configDB), Eq(config)))
      .Times(1)
      .After(read);

    // Loading should never generate onDirty notifications.
    EXPECT_CALL(observer(), onDirty(Ref(configDB))).Times(0);

    // Read the empty database.
    EXPECT_EQ(configDB.read(ioContext()), API_OK);

    // Tag mapping should've been removed.
    EXPECT_EQ(configDB.getByBackupId(config.mBackupId), nullptr);

    // Target Handle mapping should've been removed.
    EXPECT_EQ(configDB.getByRootHandle(config.mRemoteNode), nullptr);
}

TEST_F(JSONSyncConfigDBTest, ReadNoSlots)
{
    JSONSyncConfigDB configDB(dbPath(), drivePath(), observer());

    // Don't return any slots for reading.
    EXPECT_CALL(ioContext(),
                getSlotsInOrder(Eq(dbPath()), _))
      .WillOnce(Return(API_ENOENT));

    // Read should fail as there are no slots.
    EXPECT_EQ(configDB.read(ioContext()), API_ENOENT);
}

TEST_F(JSONSyncConfigDBTest, ReadUpdatesDatabase)
{
    JSONSyncConfigDB configDB(dbPath(), drivePath(), observer());

    // Add a config to the database.
    SyncConfig configBefore;

    configBefore.mExternalDrivePath = drivePath();
    configBefore.mLocalPath = Utilities::randomPath();
    configBefore.mBackupId = 1;
    configBefore.mRemoteNode = 2;

    EXPECT_NE(configDB.add(configBefore), nullptr);

    // Capture the JSON and signal write success.
    string json;

    EXPECT_CALL(ioContext(),
                write(Eq(dbPath()), _, Eq(0u)))
      .WillOnce(DoAll(SaveArg<1>(&json),
                      Return(API_OK)));

    // Write the database to disk.
    EXPECT_EQ(configDB.write(ioContext()), API_OK);

    // Change the config's target handle.
    SyncConfig configAfter = configBefore;

    configAfter.mRemoteNode = 3;

    EXPECT_NE(configDB.add(configAfter), nullptr);

    // Return a single slot for reading.
    static const vector<unsigned int> slots = {0};

    Expectation get =
      EXPECT_CALL(ioContext(),
                  getSlotsInOrder(Eq(dbPath()), _))
        .WillOnce(DoAll(SetArgReferee<1>(slots),
                        Return(API_OK)));

    // Read should return the captured JSON.
    Expectation read =
      EXPECT_CALL(ioContext(),
                  read(Eq(dbPath()), _, Eq(0u)))
        .After(get)
        .WillOnce(DoAll(SetArgReferee<1>(json),
                        Return(API_OK)));

    // Observer should be notified when the config changes.
    EXPECT_CALL(observer(),
                onChange(Ref(configDB),
                         Eq(configAfter),
                         Eq(configBefore)))
      .Times(1)
      .After(read);

    // No dirty notications should be triggered when loading.
    EXPECT_CALL(observer(), onDirty(Ref(configDB))).Times(0);

    // Read back the database.
    EXPECT_EQ(configDB.read(ioContext()), API_OK);

    // Can we still retrieve the config by tag?
    const auto* c = configDB.getByBackupId(configBefore.mBackupId);
    ASSERT_NE(c, nullptr);
    EXPECT_EQ(*c, configBefore);

    // Updated target handle mapping should no longer exist.
    EXPECT_EQ(configDB.getByRootHandle(configAfter.mRemoteNode), nullptr);

    // Original target handle mapping should be in effect.
    EXPECT_EQ(configDB.getByRootHandle(configBefore.mRemoteNode), c);
}

TEST_F(JSONSyncConfigDBTest, ReadTriesAllAvailableSlots)
{
    // Slots available for reading.
    static const vector<unsigned int> slots = {1, 2, 3};

    JSONSyncConfigDB configDB(dbPath(), drivePath(), observer());

    // Return three slots for reading.
    Expectation get =
      EXPECT_CALL(ioContext(),
                  getSlotsInOrder(Eq(dbPath()), _))
      .WillOnce(DoAll(SetArgReferee<1>(slots),
                      Return(API_OK)));

    // Attempts to read slots 1 and 2 should fail.
    Expectation read1 =
      EXPECT_CALL(ioContext(),
                  read(Eq(dbPath()), _, Eq(1u)))
      .After(get)
      .WillOnce(Return(API_EREAD));

    Expectation read2 =
      EXPECT_CALL(ioContext(),
                  read(Eq(dbPath()), _, Eq(2u)))
      .After(read1)
      .WillOnce(Return(API_EREAD));

    // Reading slot 3 should succeed.
    EXPECT_CALL(ioContext(),
                read(Eq(dbPath()), _, Eq(3u)))
      .After(read2)
      .WillOnce(DoAll(SetArgReferee<1>("[]"),
                      Return(API_OK)));

    // Read should succeed as one slot could be read.
    EXPECT_EQ(configDB.read(ioContext()), API_OK);
}

TEST_F(JSONSyncConfigDBTest, RemoveByBackupID)
{
    JSONSyncConfigDB configDB(dbPath(), drivePath(), observer());

    // Add a config to remove.
    SyncConfig config;

    config.mExternalDrivePath = drivePath();
    config.mLocalPath = Utilities::randomPath();
    config.mBackupId = 1;
    config.mRemoteNode = 2;

    EXPECT_NE(configDB.add(config), nullptr);

    // Observer should be notified when the config is removed.
    Expectation onRemove =
      EXPECT_CALL(observer(),
                  onRemove(Ref(configDB), Eq(config)))
      .Times(1);

    // Database should be dirty after config has been removed.
    EXPECT_CALL(observer(),
                onDirty(Ref(configDB)))
      .Times(1)
      .After(onRemove);

    // Remove the config by tag.
    EXPECT_EQ(configDB.removeByBackupId(config.mBackupId), API_OK);

    // Database should now be empty.
    EXPECT_TRUE(configDB.configs().empty());

    // Mappings should be removed.
    EXPECT_EQ(configDB.getByBackupId(config.mBackupId), nullptr);
    EXPECT_EQ(configDB.getByRootHandle(config.mRemoteNode), nullptr);
}

TEST_F(JSONSyncConfigDBTest, RemoveByBackupIDWhenEmpty)
{
    JSONSyncConfigDB configDB(dbPath(), drivePath(), observer());

    EXPECT_CALL(observer(), onDirty(_)).Times(0);
    EXPECT_CALL(observer(), onRemove(_, _)).Times(0);

    EXPECT_EQ(configDB.removeByBackupId(0), API_ENOENT);
}

TEST_F(JSONSyncConfigDBTest, RemoveByUnknownBackupID)
{
    JSONSyncConfigDB configDB(dbPath(), drivePath(), observer());

    // Add some config so the database isn't empty.
    {
        SyncConfig config;

        config.mExternalDrivePath = drivePath();
        config.mBackupId = 0;
        config.mRemoteNode = 1;

        EXPECT_NE(configDB.add(config), nullptr);
    }

    EXPECT_CALL(observer(), onDirty(_)).Times(0);
    EXPECT_CALL(observer(), onRemove(_, _)).Times(0);

    EXPECT_EQ(configDB.removeByBackupId(1), API_ENOENT);

    // Verify and clear the expectations now as the database will trigger
    // an onRemove notification when it is destroyed.
    Mock::VerifyAndClearExpectations(&observer());
}

TEST_F(JSONSyncConfigDBTest, RemoveByTargetHandle)
{
    JSONSyncConfigDB configDB(dbPath(), drivePath(), observer());

    // Add a config to remove.
    SyncConfig config;

    config.mExternalDrivePath = drivePath();
    config.mBackupId = 0;
    config.mRemoteNode = 1;

    EXPECT_NE(configDB.add(config), nullptr);

    // Observer should be notified when the config is removed.
    Expectation onRemove =
      EXPECT_CALL(observer(),
                  onRemove(Ref(configDB), Eq(config)))
      .Times(1);

    // Database should be dirty after the config has been removed.
    EXPECT_CALL(observer(),
                onDirty(Ref(configDB)))
      .Times(1)
      .After(onRemove);

    // Remove the config.
    EXPECT_EQ(configDB.removeByRootNode(config.mRemoteNode), API_OK);

    // Database should now be empty.
    EXPECT_TRUE(configDB.configs().empty());

    // Mappings should be removed.
    EXPECT_EQ(configDB.getByBackupId(config.mBackupId), nullptr);
    EXPECT_EQ(configDB.getByRootHandle(config.mRemoteNode), nullptr);
}


TEST_F(JSONSyncConfigDBTest, RemoveByTargetHandleWhenEmpty)
{
    JSONSyncConfigDB configDB(dbPath(), drivePath(), observer());

    EXPECT_CALL(observer(), onDirty(_)).Times(0);
    EXPECT_CALL(observer(), onRemove(_, _)).Times(0);

    const handle targetHandle = 0;
    EXPECT_EQ(configDB.removeByRootNode(targetHandle), API_ENOENT);
}

TEST_F(JSONSyncConfigDBTest, RemoveByUnknownTargetHandle)
{
    JSONSyncConfigDB configDB(dbPath(), drivePath(), observer());

    // Add a config so that the database isn't empty.
    {
        SyncConfig config;

        config.mExternalDrivePath = drivePath();
        config.mBackupId = 0;
        config.mRemoteNode = 1;

        EXPECT_NE(configDB.add(config), nullptr);
    }

    EXPECT_CALL(observer(), onDirty(_)).Times(0);
    EXPECT_CALL(observer(), onRemove(_, _)).Times(0);

    EXPECT_EQ(configDB.removeByRootNode(0), API_ENOENT);

    // Verify and clear the expectations now as the database will trigger
    // an onRemove notification when it is destroyed.
    Mock::VerifyAndClearExpectations(&observer());
}

TEST_F(JSONSyncConfigDBTest, Update)
{
    JSONSyncConfigDB configDB(dbPath(), drivePath(), observer());

    // Add a config.
    SyncConfig configBefore;

    configBefore.mExternalDrivePath = drivePath();
    configBefore.mEnabled = false;
    configBefore.mBackupId = 0;
    configBefore.mRemoteNode = 1;

    const auto* c = configDB.add(configBefore);
    ASSERT_NE(c, nullptr);
    EXPECT_EQ(*c, configBefore);

    // Update config.
    SyncConfig configAfter = configBefore;

    configAfter.mEnabled = true;

    // Observer should be notified when config changes.
    Expectation onChange =
      EXPECT_CALL(observer(),
                  onChange(Ref(configDB),
                           Eq(configBefore),
                           Eq(configAfter)))
      .Times(1);

    // Database needs a write after updating a config.
    EXPECT_CALL(observer(),
                onDirty(Ref(configDB)))
      .Times(1)
      .After(onChange);

    // Update config in the database.
    EXPECT_EQ(configDB.add(configAfter), c);
    EXPECT_EQ(*c, configAfter);

    // Can still retrieve by tag.
    EXPECT_EQ(configDB.getByBackupId(configAfter.mBackupId), c);

    // Can still retrieve by target handle.
    EXPECT_EQ(configDB.getByRootHandle(configAfter.mRemoteNode), c);
}

TEST_F(JSONSyncConfigDBTest, UpdateChangeTargetHandle)
{
    JSONSyncConfigDB configDB(dbPath(), drivePath(), observer());

    // Add config.
    SyncConfig configBefore;

    configBefore.mExternalDrivePath = drivePath();
    configBefore.mBackupId = 0;
    configBefore.mRemoteNode = 0;

    const auto* c = configDB.add(configBefore);
    ASSERT_NE(c, nullptr);
    EXPECT_EQ(*c, configBefore);

    // Update config.
    SyncConfig configAfter = configBefore;

    configAfter.mRemoteNode = 1;

    // Observer should be notified when a config changes.
    Expectation onChange =
      EXPECT_CALL(observer(),
                  onChange(Ref(configDB),
                           Eq(configBefore),
                           Eq(configAfter)))
      .Times(1);

    // Database should be dirty when a config has changed.
    EXPECT_CALL(observer(),
                onDirty(Ref(configDB)))
      .Times(1)
      .After(onChange);

    // Update the config in the database.
    EXPECT_EQ(configDB.add(configAfter), c);
    EXPECT_EQ(*c, configAfter);

    // Can still retrieve by tag.
    EXPECT_EQ(configDB.getByBackupId(configAfter.mBackupId), c);

    // Old target handle mapping has been removed.
    EXPECT_EQ(configDB.getByRootHandle(configBefore.mRemoteNode), nullptr);

    // New target handle mapping has been added.
    EXPECT_EQ(configDB.getByRootHandle(configAfter.mRemoteNode), c);
}

TEST_F(JSONSyncConfigDBTest, UpdateRemoveTargetHandle)
{
    JSONSyncConfigDB configDB(dbPath(), drivePath(), observer());

    // Add config.
    SyncConfig configBefore;

    configBefore.mExternalDrivePath = drivePath();
    configBefore.mBackupId = 0;
    configBefore.mRemoteNode = 0;

    const auto* c = configDB.add(configBefore);
    ASSERT_NE(c, nullptr);
    EXPECT_EQ(*c, configBefore);

    // Update config.
    SyncConfig configAfter = configBefore;

    configAfter.mRemoteNode = UNDEF;

    // Observer should be notified when a config changes.
    Expectation onChange =
      EXPECT_CALL(observer(),
                  onChange(Ref(configDB),
                           Eq(configBefore),
                           Eq(configAfter)))
      .Times(1);

    // Database should be dirty when a config has changed.
    EXPECT_CALL(observer(),
                onDirty(Ref(configDB)))
      .Times(1)
      .After(onChange);

    // Update the config in the database.
    EXPECT_EQ(configDB.add(configAfter), c);
    EXPECT_EQ(*c, configAfter);

    // Can still retrieve by tag.
    EXPECT_EQ(configDB.getByBackupId(configAfter.mBackupId), c);

    // Old target handle mapping has been removed.
    EXPECT_EQ(configDB.getByRootHandle(configBefore.mRemoteNode), nullptr);

    // No mapping ever exists for UNDEF target handle.
    EXPECT_EQ(configDB.getByRootHandle(UNDEF), nullptr);
}

TEST_F(JSONSyncConfigDBTest, WriteFail)
{
    JSONSyncConfigDB configDB(dbPath(), drivePath(), observer());

    // Any attempt to write to slot 0 will fail.
    EXPECT_CALL(ioContext(),
                write(Eq(dbPath()), _, Eq(0u)))
      .Times(2)
      .WillRepeatedly(Return(API_EWRITE));

    // Write will fail as we can't write to slot 0.
    EXPECT_EQ(configDB.write(ioContext()), API_EWRITE);

    // Make sure the slot number isn't incremented.
    EXPECT_EQ(configDB.write(ioContext()), API_EWRITE);
}

TEST_F(JSONSyncConfigDBTest, WriteOK)
{
    JSONSyncConfigDB configDB(dbPath(), drivePath(), observer());

    // Writes to slot 0 should succeed.
    Expectation write0 =
      EXPECT_CALL(ioContext(),
                  write(Eq(dbPath()), _, Eq(0u)))
      .WillOnce(Return(API_OK));

    // Writes to slot 1 should succeed.
    EXPECT_CALL(ioContext(),
                write(Eq(dbPath()), _, Eq(1u)))
      .After(write0)
      .WillOnce(Return(API_OK));

    // First write will dump data to slot 0.
    EXPECT_EQ(configDB.write(ioContext()), API_OK);

    // Second write will dump data to slot 1.
    EXPECT_EQ(configDB.write(ioContext()), API_OK);
}

class JSONSyncConfigStoreTest
  : public JSONSyncConfigTest
{
public:
    class ConfigStore
      : public JSONSyncConfigStore
    {
    public:
        ConfigStore(JSONSyncConfigIOContext& ioContext)
          : JSONSyncConfigStore(ioContext)
        {
            // Perform real behavior by default.
            ON_CALL(*this, onAdd(_, _))
              .WillByDefault(Invoke(this, &ConfigStore::onAddConcrete));

            ON_CALL(*this, onChange(_, _, _))
              .WillByDefault(Invoke(this, &ConfigStore::onChangeConcrete));

            ON_CALL(*this, onDirty(_))
              .WillByDefault(Invoke(this, &ConfigStore::onDirtyConcrete));

            ON_CALL(*this, onRemove(_, _))
              .WillByDefault(Invoke(this, &ConfigStore::onRemoveConcrete));
        }

        // Convenience.
        using DB = JSONSyncConfigDB;
        using Config = SyncConfig;

        MOCK_METHOD2(onAdd, void(DB&, const Config&));

        MOCK_METHOD3(onChange, void(DB&, const Config&, const Config&));

        MOCK_METHOD1(onDirty, void(DB&));

        MOCK_METHOD2(onRemove, void(DB&, const Config&));

    private:
        // Delegate to real behavior.
        void onAddConcrete(DB& db, const Config& config)
        {
            return JSONSyncConfigStore::onAdd(db, config);
        }

        void onChangeConcrete(DB& db, const Config& from, const Config& to)
        {
            return JSONSyncConfigStore::onChange(db, from, to);
        }

        void onDirtyConcrete(DB& db)
        {
            return JSONSyncConfigStore::onDirty(db);
        }

        void onRemoveConcrete(DB& db, const Config& config)
        {
            return JSONSyncConfigStore::onRemove(db, config);
        }
    }; // ConfigStore

    JSONSyncConfigStoreTest()
      : JSONSyncConfigTest()
    {
    }
}; // JSONSyncConfigStoreTest

// Matches a database with a specific path.
MATCHER_P(DB, drivePath, "")
{
    return arg.drivePath() == drivePath;
}

TEST_F(JSONSyncConfigStoreTest, Add)
{
    // Make sure database is removed.
    Directory drive(fsAccess(), Utilities::randomPath());

    // Create database.
    FakeStrictMock<ConfigStore> store(ioContext());

    EXPECT_NE(store.create(drive), nullptr);

    // Verify database is open.
    EXPECT_NE(store.configs(drive), nullptr);
    EXPECT_TRUE(store.opened(drive));

    // Create config to add to the database.
    SyncConfig config;

    config.mExternalDrivePath = drive;
    config.mBackupId = 1;
    config.mRemoteNode = 2;

    // onAdd should be generated when a new config is added.
    Expectation onAdd =
      EXPECT_CALL(store,
                  onAdd(DB(drive.path()), Eq(config)))
        .Times(1);

    // onDirty should be generated when a database changes.
    EXPECT_CALL(store,
                onDirty(DB(drive.path())))
      .Times(1)
      .After(onAdd);

    // Add the config to the database.
    const auto* c = store.add(config);

    ASSERT_NE(c, nullptr);
    EXPECT_EQ(*c, config);

    // Has the database been soiled?
    EXPECT_TRUE(store.dirty());

    // Can we retrieve the config by tag?
    EXPECT_EQ(store.getByBackupId(config.mBackupId), c);

    // Can we retrieve the config by target handle?
    EXPECT_EQ(store.getByRootHandle(config.mRemoteNode), c);
}

TEST_F(JSONSyncConfigStoreTest, AddDenormalized)
{
    // Make sure database is removed.
    Directory drive(fsAccess(), Utilities::randomPath());

    // Create store.
    FakeNiceMock<ConfigStore> store(ioContext());

    // Create database (using normalized path.)
    ASSERT_NE(store.create(drive), nullptr);

    // Create a normalized config.
    SyncConfig configN;

    configN.mExternalDrivePath = drive;
    configN.mLocalPath = Utilities::randomPath();
    configN.mBackupId = 1;
    configN.mRemoteNode = 2;

    // Create a denormalized config.
    SyncConfig configDN = configN;

    configDN.mExternalDrivePath.append(Utilities::separator());
    configDN.mLocalPath.append(Utilities::separator());

    // Add the denormalized config.
    const auto* c = store.add(configDN);
    ASSERT_NE(c, nullptr);

    // Config should've been normalized when it was added to the database.
    EXPECT_EQ(*c, configN);
}

TEST_F(JSONSyncConfigStoreTest, AddToUnknownDatabase)
{
    FakeStrictMock<ConfigStore> store(ioContext());

    // No attempt should be made to open an unknown database.
    EXPECT_CALL(ioContext(), getSlotsInOrder(_, _)).Times(0);
    EXPECT_CALL(ioContext(), read(_, _, _)).Times(0);
    EXPECT_CALL(ioContext(), write(_, _, _)).Times(0);

    // Create a config to add to the store.
    SyncConfig config;

    config.mExternalDrivePath = Utilities::randomPath();

    // Can't add a config to an unknown database.
    EXPECT_EQ(store.add(config), nullptr);

    // Database should remain unknown.
    EXPECT_EQ(store.configs(config.mExternalDrivePath), nullptr);
    EXPECT_FALSE(store.opened(config.mExternalDrivePath));

    // Store should still have no configs.
    EXPECT_TRUE(store.configs().empty());

    // Store should not be dirtied.
    EXPECT_FALSE(store.dirty());
}

TEST_F(JSONSyncConfigStoreTest, CloseAll)
{
    // Make sure databases are removed.
    Directory driveA(fsAccess(), Utilities::randomPath());
    Directory driveB(fsAccess(), Utilities::randomPath());

    // Database directory.
    auto backupPathA = driveA.path();

    backupPathA.appendWithSeparator(
      ConfigStore::BACKUP_CONFIG_DIR, false);

    // Create store.
    FakeNiceMock<ConfigStore> store(ioContext());

    // Add databases.
    EXPECT_NE(store.create(driveA), nullptr);
    EXPECT_NE(store.create(driveB), nullptr);

    // Verify databases are open.
    EXPECT_TRUE(store.opened(driveA));
    EXPECT_TRUE(store.opened(driveB));

    // Dirty the first database.
    SyncConfig config;

    config.mExternalDrivePath = driveA;
    config.mBackupId = 1;

    EXPECT_NE(store.add(config), nullptr);

    // Verify store is dirty.
    EXPECT_TRUE(store.dirty());

    // Attempts to write database A should fail.
    EXPECT_CALL(ioContext(),
                write(Eq(backupPathA), _, Eq(1u)))
      .WillOnce(Return(API_EWRITE));

    // Close all databases.
    EXPECT_EQ(store.close(), API_EWRITE);

    // Store should no longer be dirty.
    EXPECT_FALSE(store.dirty());

    // Both databases should no longer be present.
    EXPECT_EQ(store.configs(driveA), nullptr);
    EXPECT_EQ(store.configs(driveB), nullptr);
    EXPECT_FALSE(store.opened(driveA));
    EXPECT_FALSE(store.opened(driveB));

    // Config should no longer be present.
    EXPECT_EQ(store.getByBackupId(config.mBackupId), nullptr);
}

TEST_F(JSONSyncConfigStoreTest, CloseClean)
{
    // Make sure database is removed.
    Directory drive(fsAccess(), Utilities::randomPath());

    // Database directory.
    auto backupPath = drive.path();

    backupPath.appendWithSeparator(
      ConfigStore::BACKUP_CONFIG_DIR, false);

    // Create store.
    FakeNiceMock<ConfigStore> store(ioContext());

    // Create a database.
    ASSERT_NE(store.create(drive), nullptr);

    // Verify database is open.
    EXPECT_TRUE(store.opened(drive));

    // No writes should occur as the database is clean.
    EXPECT_CALL(ioContext(),
                write(Eq(backupPath), _, Eq(1u)))
      .Times(0);

    // Close the database.
    EXPECT_EQ(store.close(drive), API_OK);

    // Database should no longer be open.
    EXPECT_EQ(store.configs(drive), nullptr);
    EXPECT_FALSE(store.opened(drive));
}

TEST_F(JSONSyncConfigStoreTest, CloseDenormalized)
{
    // Make sure database is removed.
    Directory drive(fsAccess(), Utilities::randomPath());

    // Compute denormalized path.
    LocalPath drivePath = drive;
    drivePath.append(Utilities::separator());

    // Create store.
    FakeNiceMock<ConfigStore> store(ioContext());

    // Create database (using normalized path.)
    ASSERT_NE(store.create(drive), nullptr);

    // Verify database is open.
    EXPECT_TRUE(store.opened(drive));
    EXPECT_TRUE(store.opened(drivePath));

    // Close the database (using denormalized path.)
    EXPECT_EQ(store.close(drivePath), API_OK);

    // Database should no longer be open.
    EXPECT_FALSE(store.opened(drive));
    EXPECT_FALSE(store.opened(drivePath));
}

TEST_F(JSONSyncConfigStoreTest, CloseDirty)
{
    // Make sure database is removed.
    Directory drive(fsAccess(), Utilities::randomPath());

    // Database directory.
    auto backupPath = drive.path();

    backupPath.appendWithSeparator(
      ConfigStore::BACKUP_CONFIG_DIR, false);

    // Create store.
    FakeNiceMock<ConfigStore> store(ioContext());

    // Create a database.
    ASSERT_NE(store.create(drive), nullptr);

    // Verify database is open.
    EXPECT_TRUE(store.opened(drive));

    // Add a config to the database.
    SyncConfig config;

    config.mExternalDrivePath = drive;
    config.mBackupId = 1;
    config.mRemoteNode = 2;

    // Verify config has been added to database.
    const auto* c = store.add(config);

    ASSERT_NE(c, nullptr);
    EXPECT_EQ(*c, config);

    // Verify config is accessible.
    EXPECT_EQ(store.getByBackupId(config.mBackupId), c);
    EXPECT_EQ(store.getByRootHandle(config.mRemoteNode), c);

    // Verify database is dirty.
    EXPECT_TRUE(store.dirty());

    // A single write should be issued to update the dirty database.
    Expectation write =
      EXPECT_CALL(ioContext(),
                  write(Eq(backupPath), _, Eq(1u)))
        .Times(1);

    // onRemove should be generated when the database's config is removed.
    EXPECT_CALL(store,
                onRemove(DB(drive.path()), Eq(config)))
      .Times(1)
      .After(write);

    // Close the database.
    EXPECT_EQ(store.close(drive), API_OK);

    // Database should no longer be available.
    EXPECT_EQ(store.configs(drive), nullptr);
    EXPECT_FALSE(store.opened(drive));

    // Config should no longer be accessible.
    EXPECT_EQ(store.getByBackupId(config.mBackupId), nullptr);
    EXPECT_EQ(store.getByRootHandle(config.mRemoteNode), nullptr);
}

TEST_F(JSONSyncConfigStoreTest, CloseDirtyCantWrite)
{
    // Make sure database is removed.
    Directory drive(fsAccess(), Utilities::randomPath());
    
    // Database directory.
    auto backupPath = drive.path();

    backupPath.appendWithSeparator(
      ConfigStore::BACKUP_CONFIG_DIR, false);

    // Create store.
    FakeNiceMock<ConfigStore> store(ioContext());

    // Create database.
    ASSERT_NE(store.create(drive), nullptr);

    // Add a config so the database is dirty.
    SyncConfig config;

    config.mExternalDrivePath = drive;
    config.mBackupId = 1;
    config.mRemoteNode = 2;

    const auto* c = store.add(config);
    ASSERT_NE(c, nullptr);
    EXPECT_EQ(*c, config);

    // Make sure config's been added.
    EXPECT_EQ(store.getByBackupId(config.mBackupId), c);
    EXPECT_EQ(store.getByRootHandle(config.mRemoteNode), c);

    // Make sure database's dirty.
    EXPECT_TRUE(store.dirty());

    // Attempts to write the database should fail.
    EXPECT_CALL(ioContext(),
                write(Eq(backupPath), _, Eq(1u)))
      .Times(1)
      .WillOnce(Return(API_EWRITE));

    // Close the database.
    EXPECT_EQ(store.close(drive), API_EWRITE);

    // Database should be removed even though we couldn't flush it to disk.
    EXPECT_EQ(store.configs(drive), nullptr);
    EXPECT_FALSE(store.opened(drive));

    // Store should no longer be dirty.
    EXPECT_FALSE(store.dirty());

    // Config should no longer be accessible.
    EXPECT_EQ(store.getByBackupId(config.mBackupId), nullptr);
    EXPECT_EQ(store.getByRootHandle(config.mRemoteNode), nullptr);
}

TEST_F(JSONSyncConfigStoreTest, CloseNoDatabases)
{
    FakeStrictMock<ConfigStore> store(ioContext());

    // No attempts should be made to write any database.
    EXPECT_CALL(ioContext(), write(_, _, _)).Times(0);

    // No databases, no writing, no possible error.
    EXPECT_EQ(store.close(), API_OK);
}

TEST_F(JSONSyncConfigStoreTest, CloseUnknownDatabase)
{
    FakeStrictMock<ConfigStore> store(ioContext());

    // No attempt should be made to write the database.
    EXPECT_CALL(ioContext(), write(_, _, _)).Times(0);

    const auto drivePath = Utilities::randomPath();

    // Can't close an unknown database.
    EXPECT_EQ(store.close(drivePath), API_ENOENT);

    // Database should remain unknown.
    EXPECT_EQ(store.configs(drivePath), nullptr);
    EXPECT_FALSE(store.opened(drivePath));
}

TEST_F(JSONSyncConfigStoreTest, Configs)
{
    // Make sure databases are removed.
    Directory driveA(fsAccess(), Utilities::randomPath());
    Directory driveB(fsAccess(), Utilities::randomPath());

    FakeNiceMock<ConfigStore> store(ioContext());

    // Add a couple databases.
    const auto* dA = store.create(driveA);
    const auto* dB = store.create(driveB);

    EXPECT_EQ(store.configs(driveA), dA);
    EXPECT_EQ(store.configs(driveB), dB);
    EXPECT_TRUE(store.opened(driveA));
    EXPECT_TRUE(store.opened(driveB));

    // Add a couple configs.
    SyncConfig configA;
    SyncConfig configB;

    configA.mExternalDrivePath = driveA;
    configA.mBackupId = 1;
    configB.mExternalDrivePath = driveB;
    configB.mBackupId = 2;

    EXPECT_NE(store.add(configA), nullptr);
    EXPECT_NE(store.add(configB), nullptr);

    // Has configA been added to database A?
    ASSERT_NE(dA, nullptr);
    ASSERT_EQ(dA->size(), 1);
    EXPECT_EQ(dA->at(configA.mBackupId), configA);

    // Has configB been added to database B?
    ASSERT_NE(dB, nullptr);
    ASSERT_EQ(dB->size(), 1);
    EXPECT_EQ(dB->at(configB.mBackupId), configB);

    // Can we retrieve all configs in a single call?
    const auto configs = store.configs();

    EXPECT_EQ(configs.size(), 2);
    EXPECT_EQ(configs.at(configA.mBackupId), configA);
    EXPECT_EQ(configs.at(configB.mBackupId), configB);
}

TEST_F(JSONSyncConfigStoreTest, ConfigsDenormalized)
{
    // Make sure database is removed.
    Directory drive(fsAccess(), Utilities::randomPath());

    // Compute denormalized path.
    LocalPath drivePath = drive;
    drivePath.append(Utilities::separator());

    // Create store.
    FakeNiceMock<ConfigStore> store(ioContext());

    // Create database (using normalized path.)
    auto* configs = store.create(drive);
    EXPECT_NE(configs, nullptr);

    // Verify database is open.
    EXPECT_TRUE(store.opened(drive));
    EXPECT_TRUE(store.opened(drivePath));

    // Retrieve configs using denormalized path.
    EXPECT_EQ(store.configs(drivePath), configs);
}

TEST_F(JSONSyncConfigStoreTest, ConfigsNoDatabases)
{
    FakeStrictMock<ConfigStore> store(ioContext());

    EXPECT_TRUE(store.configs().empty());
}

TEST_F(JSONSyncConfigStoreTest, ConfigsUnknownDatabase)
{
    FakeStrictMock<ConfigStore> store(ioContext());

    // No attempt should be made to open an unknown database.
    EXPECT_CALL(ioContext(), getSlotsInOrder(_, _)).Times(0);
    EXPECT_CALL(ioContext(), read(_, _, _)).Times(0);

    const auto drivePath = Utilities::randomPath();

    // No database? No configs.
    EXPECT_EQ(store.configs(drivePath), nullptr);
}

TEST_F(JSONSyncConfigStoreTest, Create)
{
    const auto drivePath = Utilities::randomPath();

    // Database directory.
    auto backupPath = drivePath;

    backupPath.appendWithSeparator(
      ConfigStore::BACKUP_CONFIG_DIR, false);

    // No slots available for reading.
    Expectation get =
      EXPECT_CALL(ioContext(),
                  getSlotsInOrder(Eq(backupPath), _))
        .WillOnce(Return(API_ENOENT));

    // Initial write should succeed.
    Expectation write =
      EXPECT_CALL(ioContext(),
                  write(Eq(backupPath), Eq("[]"), Eq(0u)))
        .After(get)
        .WillOnce(Return(API_OK));

    // Prepare config store.
    FakeStrictMock<ConfigStore> store(ioContext());

    // Create the database.
    const auto* configs = store.create(drivePath);

    // Database should be marked as open.
    EXPECT_TRUE(store.opened(drivePath));

    // No configs should have been deserialized.
    ASSERT_NE(configs, nullptr);
    EXPECT_TRUE(configs->empty());

    // Can we get our hands on this database's configs?
    EXPECT_EQ(store.configs(drivePath), configs);
}

TEST_F(JSONSyncConfigStoreTest, CreateAlreadyOpened)
{
    auto drivePath = Utilities::randomPath();

    // Database directory.
    auto backupPath = drivePath;

    backupPath.appendWithSeparator(
      ConfigStore::BACKUP_CONFIG_DIR, false);

    // No slots available for reading.
    Expectation get =
      EXPECT_CALL(ioContext(),
                  getSlotsInOrder(Eq(backupPath), _))
        .WillOnce(Return(API_ENOENT));

    // Initial write should succeed.
    Expectation write =
      EXPECT_CALL(ioContext(),
                  write(Eq(backupPath), Eq("[]"), Eq(0u)))
        .After(get)
        .WillOnce(Return(API_OK));

    // Prepare config store.
    FakeStrictMock<ConfigStore> store(ioContext());

    // Create the database.
    const auto* configs = store.create(drivePath);

    // Database should be marked as open.
    EXPECT_TRUE(store.opened(drivePath));

    // No configs should have been deserialized.
    ASSERT_NE(configs, nullptr);
    EXPECT_TRUE(configs->empty());

    // Can we get our hands on this database's configs?
    EXPECT_EQ(store.configs(drivePath), configs);

    // Attempts to re-create the database should fail.
    EXPECT_EQ(store.create(drivePath), nullptr);

    // Attempts to re-open the database should fail.
    EXPECT_EQ(store.open(drivePath), nullptr);

    // Repeat the above tests with a denormalized path.
    drivePath.append(Utilities::separator());

    EXPECT_EQ(store.configs(drivePath), configs);
    EXPECT_EQ(store.create(drivePath), nullptr);
    EXPECT_EQ(store.open(drivePath), nullptr);
}

TEST_F(JSONSyncConfigStoreTest, CreateCantReadExisting)
{
    const auto drivePath = Utilities::randomPath();

    // Database directory.
    auto backupPath = drivePath;

    backupPath.appendWithSeparator(
      ConfigStore::BACKUP_CONFIG_DIR, false);

    // Return a single slot for reading.
    static const vector<unsigned int> slots = {0};

    Expectation get =
      EXPECT_CALL(ioContext(),
                  getSlotsInOrder(Eq(backupPath), _))
        .WillOnce(DoAll(SetArgReferee<1>(slots),
                        Return(API_OK)));

    // Reading the slot should fail.
    EXPECT_CALL(ioContext(),
                read(Eq(backupPath), _, Eq(0u)))
      .After(get)
      .WillOnce(Return(API_EREAD));

    FakeStrictMock<ConfigStore> store(ioContext());

    // Try and create the database.
    EXPECT_EQ(store.create(drivePath), nullptr);

    // Database should remain unknown.
    EXPECT_EQ(store.configs(drivePath), nullptr);
    EXPECT_FALSE(store.opened(drivePath));
}

TEST_F(JSONSyncConfigStoreTest, CreateCantWrite)
{
    const auto drivePath = Utilities::randomPath();

    // Database directory.
    auto backupPath = drivePath;

    backupPath.appendWithSeparator(
      ConfigStore::BACKUP_CONFIG_DIR, false);

    // No slots available for reading.
    Expectation get =
      EXPECT_CALL(ioContext(),
                  getSlotsInOrder(Eq(backupPath), _))
        .WillOnce(Return(API_ENOENT));

    // Initial write should fail.
    EXPECT_CALL(ioContext(),
                write(Eq(backupPath), Eq("[]"), Eq(0u)))
      .After(get)
      .WillOnce(Return(API_EWRITE));

    // Prepare config store.
    FakeStrictMock<ConfigStore> store(ioContext());

    // Try and create the database.
    EXPECT_EQ(store.create(drivePath), nullptr);

    // Database should remain unknown.
    EXPECT_EQ(store.configs(drivePath), nullptr);
    EXPECT_FALSE(store.opened(drivePath));

    // Store should remain unsoiled.
    EXPECT_FALSE(store.dirty());
}

TEST_F(JSONSyncConfigStoreTest, CreateExisting)
{
    const auto drivePath = Utilities::randomPath();

    // Database directory.
    auto backupPath = drivePath;

    backupPath.appendWithSeparator(
      ConfigStore::BACKUP_CONFIG_DIR, false);

    JSONSyncConfigMap written;

    // Populate database.
    {
        SyncConfig config;

        config.mExternalDrivePath = drivePath;
        config.mBackupId = 1;
        config.mRemoteNode = 2;

        written.emplace(config.mBackupId, config);
    }

    // Serialize database to JSON.
    JSONWriter writer;

    ioContext().serialize(written, writer);

    // Return a single slot for reading.
    static const vector<unsigned int> slots = {0};

    Expectation get =
      EXPECT_CALL(ioContext(),
                  getSlotsInOrder(Eq(backupPath), _))
        .WillOnce(DoAll(SetArgReferee<1>(slots),
                        Return(API_OK)));

    // Reading the slot should return the generated JSON.
    Expectation read =
      EXPECT_CALL(ioContext(),
                  read(Eq(backupPath), _, Eq(0u)))
        .After(get)
        .WillOnce(DoAll(SetArgReferee<1>(writer.getstring()),
                        Return(API_OK)));

    // No write should be generated when loading an existing database.
    EXPECT_CALL(ioContext(), write(Eq(drivePath), _, _)) .Times(0);

    // Prepare config store.
    FakeNiceMock<ConfigStore> store(ioContext());

    // onAdd should be generated for each config loaded from disk.
    EXPECT_CALL(store,
                onAdd(DB(drivePath), Eq(written.at(1))))
      .Times(1)
      .After(read);

    // onDirty should never be generated by a load.
    EXPECT_CALL(store, onDirty(_)).Times(0);

    // Try creating the database.
    const auto* configs = store.create(drivePath);

    // The database should now be considered open.
    EXPECT_TRUE(store.opened(drivePath));

    // Store should never be dirtied by a load.
    EXPECT_FALSE(store.dirty());

    // Was the existing database correctly deserialized?
    ASSERT_NE(configs, nullptr);
    EXPECT_EQ(*configs, written);

    // Can we retrieve this databases configs?
    EXPECT_EQ(store.configs(drivePath), configs);

    // Can we retrieve the config by tag?
    EXPECT_EQ(store.getByBackupId(configs->begin()->first),
              &configs->begin()->second);
    
    // Can we retrieve the config by target handle?
    EXPECT_EQ(store.getByRootHandle(configs->begin()->second.mRemoteNode),
              &configs->begin()->second);
}

TEST_F(JSONSyncConfigStoreTest, Destruct)
{
    // Nested scope so we can test destruction.
    {
        // Make sure database is removed.
        Directory drive(fsAccess(), Utilities::randomPath());

        // Database directory.
        auto backupPath = drive.path();

        backupPath.appendWithSeparator(
          ConfigStore::BACKUP_CONFIG_DIR, false);

        // Create store.
        FakeNiceMock<ConfigStore> store(ioContext());

        // Create database.
        EXPECT_NE(store.create(drive), nullptr);

        // Dirty database.
        SyncConfig config;

        config.mExternalDrivePath = drive;
        config.mBackupId = 1;

        EXPECT_NE(store.add(config), nullptr);
        
        // Verify store is dirty.
        EXPECT_TRUE(store.dirty());

        // Database should be flushed when the store is destroyed.
        EXPECT_CALL(ioContext(),
                    write(Eq(backupPath), _, _))
          .Times(1);
    }
}

TEST_F(JSONSyncConfigStoreTest, FlushAll)
{
    const auto& BACKUP_DIR = ConfigStore::BACKUP_CONFIG_DIR;

    // Make sure databases are removed.
    Directory driveA(fsAccess(), Utilities::randomPath());
    Directory driveB(fsAccess(), Utilities::randomPath());

    // Database directories.
    auto backupPathA = driveA.path();
    auto backupPathB = driveB.path();

    backupPathA.appendWithSeparator(BACKUP_DIR, false);
    backupPathB.appendWithSeparator(BACKUP_DIR, false);

    // Create store.
    FakeNiceMock<ConfigStore> store(ioContext());

    // Add databases.
    EXPECT_NE(store.create(driveA), nullptr);
    EXPECT_NE(store.create(driveB), nullptr);

    // Dirty databases.
    SyncConfig configA;
    SyncConfig configB;

    configA.mExternalDrivePath = driveA;
    configA.mBackupId = 1;
    configB.mExternalDrivePath = driveB;
    configB.mBackupId = 2;

    EXPECT_NE(store.add(configA), nullptr);
    EXPECT_NE(store.add(configB), nullptr);

    // Verify store is dirty.
    EXPECT_TRUE(store.dirty());

    // Attempts to flush database A should fail.
    EXPECT_CALL(ioContext(),
                write(Eq(backupPathA), _, _))
      .WillOnce(Return(API_EWRITE));

    // Attempts to flush database B should succeed.
    EXPECT_CALL(ioContext(),
                write(Eq(backupPathB), _, _))
      .Times(1);

    // Flush the databases.
    vector<LocalPath> drives;

    EXPECT_EQ(store.flush(drives), API_EWRITE);

    // Have we captured the fact that database A couldn't be flushed?
    ASSERT_EQ(drives.size(), 1);
    EXPECT_EQ(drives.back(), driveA);

    // Store should be clean regardless of flush failures.
    EXPECT_FALSE(store.dirty());
}

TEST_F(JSONSyncConfigStoreTest, FlushDenormalized)
{
    // Make sure database is removed.
    Directory drive(fsAccess(), Utilities::randomPath());

    // Database path.
    auto backupPath = drive.path();

    backupPath.appendWithSeparator(
      ConfigStore::BACKUP_CONFIG_DIR, false);

    // Compute denormalized drive path.
    LocalPath drivePath = drive;
    drivePath.append(Utilities::separator());

    // Create store.
    FakeNiceMock<ConfigStore> store(ioContext());

    // Create database (using normalized path.)
    EXPECT_NE(store.create(drive), nullptr);

    // Dirty the database.
    SyncConfig config;

    config.mExternalDrivePath = drive;
    config.mBackupId = 1;

    EXPECT_NE(store.add(config), nullptr);

    // Verify database is dirty.
    EXPECT_TRUE(store.dirty());

    // Make sure database is flushed.
    EXPECT_CALL(ioContext(),
                write(Eq(backupPath), _, _))
      .Times(1);

    // Flush the database (using denormalized path.)
    EXPECT_EQ(store.flush(drivePath), API_OK);
    
    // Store should no longer be dirty.
    EXPECT_FALSE(store.dirty());

    // Verify now as the store will write databases upon destruction.
    Mock::VerifyAndClearExpectations(&ioContext());
}

TEST_F(JSONSyncConfigStoreTest, FlushFail)
{
    // Make sure database is removed.
    Directory drive(fsAccess(), Utilities::randomPath());

    // Database path.
    auto backupPath = drive.path();

    backupPath.appendWithSeparator(
      ConfigStore::BACKUP_CONFIG_DIR, false);

    // Create store.
    FakeNiceMock<ConfigStore> store(ioContext());

    // Add database.
    EXPECT_NE(store.create(drive), nullptr);

    // Dirty database.
    SyncConfig config;

    config.mExternalDrivePath = drive;
    config.mRemoteNode = 1;

    EXPECT_NE(store.add(config), nullptr);
    EXPECT_TRUE(store.dirty());

    // Attempts to write to database A should fail.
    EXPECT_CALL(ioContext(),
                write(Eq(backupPath), _, _))
      .WillOnce(Return(API_EWRITE));

    // Flushing the database should fail.
    EXPECT_EQ(store.flush(drive), API_EWRITE);

    // Regardless, store should no longer be dirty.
    EXPECT_FALSE(store.dirty());
}

TEST_F(JSONSyncConfigStoreTest, FlushSpecific)
{
    const auto& BACKUP_DIR = ConfigStore::BACKUP_CONFIG_DIR;

    // Make sure databases are removed.
    Directory driveA(fsAccess(), Utilities::randomPath());
    Directory driveB(fsAccess(), Utilities::randomPath());

    // Database directories.
    auto backupPathA = driveA.path();
    auto backupPathB = driveB.path();

    backupPathA.appendWithSeparator(BACKUP_DIR, false);
    backupPathB.appendWithSeparator(BACKUP_DIR, false);

    // Create store.
    FakeNiceMock<ConfigStore> store(ioContext());

    // Create databases.
    EXPECT_NE(store.create(driveA), nullptr);
    EXPECT_NE(store.create(driveB), nullptr);

    // Dirty both databases.
    SyncConfig configA;
    SyncConfig configB;

    configA.mExternalDrivePath = driveA;
    configA.mBackupId = 1;
    configB.mExternalDrivePath = driveB;
    configB.mBackupId = 2;

    EXPECT_NE(store.add(configA), nullptr);
    EXPECT_NE(store.add(configB), nullptr);

    // Verify databases are dirty.
    EXPECT_TRUE(store.dirty());

    // Flushing should trigger a write to database A.
    EXPECT_CALL(ioContext(),
                write(Eq(backupPathA), _, _))
      .Times(1);

    // But since we're being specific, none for database B.
    EXPECT_CALL(ioContext(),
                write(Eq(backupPathB), _, _))
      .Times(0);

    // Flush database A.
    EXPECT_EQ(store.flush(driveA), API_OK);

    // Database B is still dirty.
    EXPECT_TRUE(store.dirty());

    // Flush database A again.
    // This should be a no-op as it is clean.
    EXPECT_EQ(store.flush(driveA), API_OK);

    // Verify (and clear) expectations now as database B will be flushed
    // when the store is destroyed.
    Mock::VerifyAndClearExpectations(&ioContext());
}

TEST_F(JSONSyncConfigStoreTest, FlushNoDatabases)
{
    FakeStrictMock<ConfigStore> store(ioContext());

    // No attempts should be made to write any database.
    EXPECT_CALL(ioContext(), write(_, _, _)).Times(0);

    // No databases, no writing, no possible error.
    EXPECT_EQ(store.flush(), API_OK);
}

TEST_F(JSONSyncConfigStoreTest, FlushUnknownDatabase)
{
    FakeStrictMock<ConfigStore> store(ioContext());

    // No attempt should be made to write the database.
    EXPECT_CALL(ioContext(), write(_, _, _)).Times(0);

    const auto drivePath = Utilities::randomPath();

    // Can't flush an unknown database.
    EXPECT_EQ(store.flush(drivePath), API_ENOENT);

    // Database should remain unknown.
    EXPECT_EQ(store.configs(drivePath), nullptr);
    EXPECT_FALSE(store.opened(drivePath));
}

TEST_F(JSONSyncConfigStoreTest, Open)
{
    auto drivePath = Utilities::randomPath();

    // Database path.
    auto backupPath = drivePath;

    backupPath.appendWithSeparator(
      ConfigStore::BACKUP_CONFIG_DIR, false);

    JSONSyncConfigMap written;

    // Populate database.
    {
        SyncConfig config;

        config.mExternalDrivePath = drivePath;
        config.mBackupId = 1;
        config.mRemoteNode = 2;

        written.emplace(config.mBackupId, config);
    }

    // Serialize database to JSON.
    JSONWriter writer;

    ioContext().serialize(written, writer);

    // Return a single slot for reading.
    static const vector<unsigned int> slots = {0};

    Expectation get =
      EXPECT_CALL(ioContext(),
                  getSlotsInOrder(Eq(backupPath), _))
        .Times(1)
        .WillOnce(DoAll(SetArgReferee<1>(slots),
                        Return(API_OK)));

    // Return the JSON on read and signal success.
    Expectation read =
      EXPECT_CALL(ioContext(),
                  read(Eq(backupPath), _, Eq(0u)))
        .Times(1)
        .After(get)
        .WillOnce(DoAll(SetArgReferee<1>(writer.getstring()),
                        Return(API_OK)));

    // Create the store.
    FakeStrictMock<ConfigStore> store(ioContext());

    // onAdd should be generated when we add a config to the store.
    EXPECT_CALL(store,
                onAdd(DB(drivePath), Eq(written.at(1))))
      .Times(1)
      .After(read);

    // Open the database.
    const auto* configs = store.open(drivePath);

    ASSERT_NE(configs, nullptr);
    EXPECT_EQ(*configs, written);

    // Verify database is open.
    EXPECT_EQ(store.configs(drivePath), configs);
    EXPECT_TRUE(store.opened(drivePath));

    // Can we retrieve the loaded config by tag?
    EXPECT_EQ(store.getByBackupId(configs->begin()->first),
              &configs->begin()->second);

    // Can we retrieve the loaded config by target handle?
    EXPECT_EQ(store.getByRootHandle(configs->begin()->second.mRemoteNode),
              &configs->begin()->second);

    // Shouldn't be able to create an already open database.
    EXPECT_EQ(store.create(drivePath), nullptr);

    // Shouldn't be able to open an already open database.
    EXPECT_EQ(store.open(drivePath), nullptr);

    // Repeat the above two tests with a denormalized path.
    drivePath.append(Utilities::separator());

    EXPECT_EQ(store.create(drivePath), nullptr);
    EXPECT_EQ(store.open(drivePath), nullptr);
}

TEST_F(JSONSyncConfigStoreTest, OpenCantRead)
{
    const auto drivePath = Utilities::randomPath();

    // Database directory.
    auto backupPath = drivePath;

    backupPath.appendWithSeparator(
      ConfigStore::BACKUP_CONFIG_DIR, false);

    // A single slot available for reading.
    static const vector<unsigned int> slots = {0};

    Expectation get =
      EXPECT_CALL(ioContext(),
                  getSlotsInOrder(Eq(backupPath), _))
        .Times(1)
        .WillOnce(DoAll(SetArgReferee<1>(slots),
                        Return(API_OK)));

    // Attempts to read the slot should fail.
    EXPECT_CALL(ioContext(),
                read(Eq(backupPath), _, Eq(0u)))
      .Times(1)
      .After(get)
      .WillOnce(Return(API_EREAD));

    // Create the store.
    FakeStrictMock<ConfigStore> store(ioContext());

    // Try and open the database.
    EXPECT_EQ(store.open(drivePath), nullptr);

    // Store should not be soiled.
    EXPECT_FALSE(store.dirty());

    // Database should remain unknown.
    EXPECT_EQ(store.configs(drivePath), nullptr);
    EXPECT_FALSE(store.opened(drivePath));
}

TEST_F(JSONSyncConfigStoreTest, OpenNoDatabase)
{
    const auto drivePath = Utilities::randomPath();

    // Database directory.
    auto backupPath = drivePath;

    backupPath.appendWithSeparator(
      ConfigStore::BACKUP_CONFIG_DIR, false);

    // No slots available for reading.
    EXPECT_CALL(ioContext(),
                getSlotsInOrder(Eq(backupPath), _))
      .Times(1)
      .WillOnce(Return(API_ENOENT));

    // Create store.
    FakeStrictMock<ConfigStore> store(ioContext());

    // Try and open the database.
    EXPECT_EQ(store.open(drivePath), nullptr);

    // Store should not be dirty.
    EXPECT_FALSE(store.dirty());

    // Database should remain unknown.
    EXPECT_EQ(store.configs(drivePath), nullptr);
    EXPECT_FALSE(store.opened(drivePath));
}

TEST_F(JSONSyncConfigStoreTest, OpenedDenormalized)
{
    // Make sure database is removed.
    Directory drive(fsAccess(), Utilities::randomPath());

    // Compute denormalized path.
    LocalPath drivePath = drive;
    drivePath.append(Utilities::separator());

    // Create store.
    FakeNiceMock<ConfigStore> store(ioContext());

    // Create the database (using normalized path.)
    ASSERT_NE(store.create(drive), nullptr);

    // Check database is open (using normalized path.)
    EXPECT_TRUE(store.opened(drive));

    // Check database is open (using denormalized path.)
    EXPECT_TRUE(store.opened(drivePath));
}

TEST_F(JSONSyncConfigStoreTest, OpenedUnknownDatabase)
{
    FakeStrictMock<ConfigStore> store(ioContext());

    // No attempt should be made to read an unknown database.
    EXPECT_CALL(ioContext(), read(_, _, _)).Times(0);

    EXPECT_FALSE(store.opened(Utilities::randomPath()));
}

TEST_F(JSONSyncConfigStoreTest, RemoveByBackupID)
{
    // Make sure database is removed.
    Directory drive(fsAccess(), Utilities::randomPath());

    // Create store.
    FakeNiceMock<ConfigStore> store(ioContext());

    // Create database.
    EXPECT_NE(store.create(drive), nullptr);

    // Verify database is open.
    EXPECT_TRUE(store.opened(drive));

    // Add config to store.
    SyncConfig config;

    config.mExternalDrivePath = drive;
    config.mBackupId = 1;
    config.mRemoteNode = 2;

    EXPECT_NE(store.add(config), nullptr);

    // Flush to make sure database isn't dirty.
    EXPECT_EQ(store.flush(), API_OK);
    EXPECT_FALSE(store.dirty());

    // onRemove should be generated when we remove a config.
    Expectation onRemove =
      EXPECT_CALL(store,
                  onRemove(DB(drive.path()), Eq(config)))
        .Times(1);

    // onDirty should be generated when a database changes.
    EXPECT_CALL(store,
                onDirty(DB(drive.path())))
      .Times(1)
      .After(onRemove);

    // Remove the config.
    EXPECT_EQ(store.removeByBackupId(config.mBackupId), API_OK);

    // Database should be soiled.
    EXPECT_TRUE(store.dirty());

    // Mappings should be invalidated.
    EXPECT_EQ(store.getByBackupId(config.mBackupId), nullptr);
    EXPECT_EQ(store.getByRootHandle(config.mRemoteNode), nullptr);
}

TEST_F(JSONSyncConfigStoreTest, RemoveByTargetHandle)
{
    // Make sure database is removed.
    Directory drive(fsAccess(), Utilities::randomPath());

    // Create store.
    FakeNiceMock<ConfigStore> store(ioContext());

    // Create database.
    EXPECT_NE(store.create(drive), nullptr);

    // Verify database is open.
    EXPECT_TRUE(store.opened(drive));

    // Add config to store.
    SyncConfig config;

    config.mExternalDrivePath = drive;
    config.mBackupId = 2;
    config.mRemoteNode = 3;

    EXPECT_NE(store.add(config), nullptr);

    // Flush to make sure database isn't dirty.
    EXPECT_EQ(store.flush(), API_OK);
    EXPECT_FALSE(store.dirty());

    // onRemove should be generated when we remove a config.
    Expectation onRemove =
      EXPECT_CALL(store,
                  onRemove(DB(drive.path()), Eq(config)))
        .Times(1);

    // onDirty should be generated when a database changes.
    EXPECT_CALL(store,
                onDirty(DB(drive.path())))
      .Times(1)
      .After(onRemove);

    // Remove the config.
    EXPECT_EQ(store.removeByRootNode(config.mRemoteNode), API_OK);

    // Database should be soiled.
    EXPECT_TRUE(store.dirty());

    // Mappings should be invalidated.
    EXPECT_EQ(store.getByBackupId(config.mBackupId), nullptr);
    EXPECT_EQ(store.getByRootHandle(config.mRemoteNode), nullptr);
}

TEST_F(JSONSyncConfigStoreTest, RemoveUnknownBackupID)
{
    FakeStrictMock<ConfigStore> store(ioContext());

    // There should be no attempts to write any database.
    EXPECT_CALL(ioContext(), write(_, _, _)).Times(0);

    // Can't remove something we don't know about.
    EXPECT_EQ(store.removeByBackupId(0), API_ENOENT);

    // No change? Not dirty.
    EXPECT_FALSE(store.dirty());
}

TEST_F(JSONSyncConfigStoreTest, RemoveUnknownTargetHandle)
{
    FakeStrictMock<ConfigStore> store(ioContext());

    // There should be no attempts to write any database.
    EXPECT_CALL(ioContext(), write(_, _, _)).Times(0);

    // Can't remove something we don't know about.
    EXPECT_EQ(store.removeByRootNode(0), API_ENOENT);

    // No change? Not dirty.
    EXPECT_FALSE(store.dirty());
}

TEST_F(JSONSyncConfigStoreTest, Update)
{
    // Make sure database is removed.
    Directory drive(fsAccess(), Utilities::randomPath());

    // Create store.
    FakeNiceMock<ConfigStore> store(ioContext());

    // Create database.
    EXPECT_NE(store.create(drive), nullptr);
    EXPECT_NE(store.configs(drive), nullptr);
    EXPECT_TRUE(store.opened(drive));

    // Create config to add to database.
    SyncConfig configBefore;

    configBefore.mExternalDrivePath = drive;
    configBefore.mBackupId = 1;
    configBefore.mRemoteNode = 2;

    // Add config to database.
    const auto* c = store.add(configBefore);

    ASSERT_NE(c, nullptr);
    EXPECT_EQ(*c, configBefore);

    // Verify config has been added to database.
    EXPECT_EQ(store.getByBackupId(configBefore.mBackupId), c);
    EXPECT_EQ(store.getByRootHandle(configBefore.mRemoteNode), c);

    // Make sure database is clean.
    EXPECT_EQ(store.flush(), API_OK);

    // Update config.
    SyncConfig configAfter = configBefore;

    configAfter.mLocalPath = Utilities::randomPath();

    // onChange should be generated when a config changes.
    Expectation onChange =
      EXPECT_CALL(store,
                  onChange(DB(drive.path()),
                           Eq(configBefore),
                           Eq(configAfter)))
        .Times(1);

    // onDirty should be generated when the database changes.
    EXPECT_CALL(store,
                onDirty(DB(drive.path())))
      .Times(1)
      .After(onChange);

    // Update the config.
    EXPECT_EQ(store.add(configAfter), c);

    // Verify config has been updated.
    EXPECT_EQ(*c, configAfter);

    // Database should be soiled.
    EXPECT_TRUE(store.dirty());

    // Is the config still accessible by tag?
    EXPECT_EQ(store.getByBackupId(configAfter.mBackupId), c);

    // Is the config still accessible by target handle?
    EXPECT_EQ(store.getByRootHandle(configAfter.mRemoteNode), c);
}

TEST_F(JSONSyncConfigStoreTest, UpdateChangeDrivePath)
{
    // Make sure databases are removed.
    Directory driveA(fsAccess(), Utilities::randomPath());
    Directory driveB(fsAccess(), Utilities::randomPath());

    // Create store.
    FakeNiceMock<ConfigStore> store(ioContext());

    // Create databases.
    EXPECT_NE(store.create(driveA), nullptr);
    EXPECT_NE(store.create(driveB), nullptr);

    // Verify databases are open.
    ASSERT_NE(store.configs(driveA), nullptr);
    ASSERT_NE(store.configs(driveB), nullptr);
    EXPECT_TRUE(store.opened(driveA));
    EXPECT_TRUE(store.opened(driveB));

    // Create config.
    SyncConfig configBefore;

    configBefore.mExternalDrivePath = driveA;
    configBefore.mBackupId = 1;
    configBefore.mRemoteNode = 2;

    // Add config to database A.
    const auto* cA = store.add(configBefore);
    ASSERT_NE(cA, nullptr);
    EXPECT_EQ(*cA, configBefore);

    // Database A should be dirty.
    EXPECT_TRUE(store.dirty());

    // Make sure config is accessible.
    EXPECT_EQ(store.getByBackupId(configBefore.mBackupId), cA);
    EXPECT_EQ(store.getByRootHandle(configBefore.mRemoteNode), cA);

    // Flush database so store is clean.
    EXPECT_EQ(store.flush(), API_OK);
    EXPECT_FALSE(store.dirty());

    // Create updated config.
    SyncConfig configAfter = configBefore;

    configAfter.mExternalDrivePath = driveB;

    // onRemove should be generated when a config is removed.
    Expectation onRemoveFromA =
      EXPECT_CALL(store,
                  onRemove(DB(driveA.path()), Eq(configBefore)))
        .Times(1);

    // onDirty should be generated when a database changes.
    Expectation onDirtyA =
      EXPECT_CALL(store,
                  onDirty(DB(driveA.path())))
        .Times(1)
        .After(onRemoveFromA);

    // onAdd should be generated when a config is added.
    Expectation onAddToB =
      EXPECT_CALL(store,
                  onAdd(DB(driveB.path()), Eq(configAfter)))
        .Times(1)
        .After(onDirtyA);

    // onDirty should be generated when a database changes.
    EXPECT_CALL(store,
                onDirty(DB(driveB.path())))
      .Times(1)
      .After(onAddToB);

    // Update the config.
    const auto* cB = store.add(configAfter);

    ASSERT_NE(cB, nullptr);
    EXPECT_EQ(*cB, configAfter);

    // Databases should be dirty.
    EXPECT_TRUE(store.dirty());

    // Database A should now be empty.
    EXPECT_TRUE(store.configs(driveA)->empty());

    // Database B should now contain a single config.
    EXPECT_EQ(store.configs(driveB)->size(), 1);

    // Config still accessible by tag?
    EXPECT_EQ(store.getByBackupId(configAfter.mBackupId), cB);

    // Config still accessible by target handle?
    EXPECT_EQ(store.getByRootHandle(configAfter.mRemoteNode), cB);
}

TEST_F(JSONSyncConfigStoreTest, UpdateChangeTargetHandle)
{
    // Make sure database is removed.
    Directory drive(fsAccess(), Utilities::randomPath());

    // Create store.
    FakeNiceMock<ConfigStore> store(ioContext());

    // Create database.
    EXPECT_NE(store.create(drive), nullptr);
    EXPECT_NE(store.configs(drive), nullptr);
    EXPECT_TRUE(store.opened(drive));

    // Create config to add to database.
    SyncConfig configBefore;

    configBefore.mExternalDrivePath = drive;
    configBefore.mBackupId = 1;
    configBefore.mRemoteNode = 2;

    // Add config to database.
    const auto* c = store.add(configBefore);

    ASSERT_NE(c, nullptr);
    EXPECT_EQ(*c, configBefore);

    // Verify config has been added to database.
    EXPECT_EQ(store.getByBackupId(configBefore.mBackupId), c);
    EXPECT_EQ(store.getByRootHandle(configBefore.mRemoteNode), c);

    // Make sure database is clean.
    EXPECT_EQ(store.flush(), API_OK);

    // Update config.
    SyncConfig configAfter = configBefore;

    configAfter.mRemoteNode = 3;

    // onChange should be generated when a config changes.
    Expectation onChange =
      EXPECT_CALL(store,
                  onChange(DB(drive.path()),
                           Eq(configBefore),
                           Eq(configAfter)))
        .Times(1);

    // onDirty should be generated when the database changes.
    EXPECT_CALL(store,
                onDirty(DB(drive.path())))
      .Times(1)
      .After(onChange);

    // Update the config.
    EXPECT_EQ(store.add(configAfter), c);

    // Verify config has been updated.
    EXPECT_EQ(*c, configAfter);

    // Database should be soiled.
    EXPECT_TRUE(store.dirty());

    // Is the config still accessible by tag?
    EXPECT_EQ(store.getByBackupId(configAfter.mBackupId), c);

    // Config should no longer be accessible by old target handle.
    EXPECT_EQ(store.getByRootHandle(configBefore.mRemoteNode), nullptr);

    // Is the config accessible under its new target handle?
    EXPECT_EQ(store.getByRootHandle(configAfter.mRemoteNode), c);
}

TEST_F(JSONSyncConfigStoreTest, UpdateChangeUnknownDrivePath)
{
    // Make sure database is removed.
    Directory drive(fsAccess(), Utilities::randomPath());

    // Create store.
    FakeNiceMock<ConfigStore> store(ioContext());

    // Create database.
    ASSERT_NE(store.create(drive), nullptr);

    // Verify database has been opened.
    ASSERT_NE(store.configs(drive), nullptr);
    EXPECT_TRUE(store.opened(drive));

    // Create config.
    SyncConfig configBefore;

    configBefore.mExternalDrivePath = drive;
    configBefore.mBackupId = 1;
    configBefore.mRemoteNode = 2;

    // Add config to database.
    const auto* c = store.add(configBefore);

    ASSERT_NE(c, nullptr);
    EXPECT_EQ(*c, configBefore);

    // Database should be soiled.
    EXPECT_TRUE(store.dirty());

    // Make sure config is accessible.
    EXPECT_EQ(store.getByBackupId(configBefore.mBackupId), c);
    EXPECT_EQ(store.getByRootHandle(configBefore.mRemoteNode), c);

    // Flush so that databases are clean.
    EXPECT_EQ(store.flush(), API_OK);

    // Create updated config.
    SyncConfig configAfter = configBefore;

    configAfter.mExternalDrivePath = Utilities::randomPath();

    // onRemove should be generated when a config is removed.
    Expectation onRemove =
      EXPECT_CALL(store,
                  onRemove(DB(drive.path()), Eq(configBefore)))
        .Times(1);
      
    // onDirty should be generated when a database is altered.
    EXPECT_CALL(store,
                onDirty(DB(drive.path())))
      .Times(1)
      .After(onRemove);

    // Move config to an unknown database.
    EXPECT_EQ(store.add(configAfter), nullptr);

    // Database should be dirty.
    EXPECT_TRUE(store.dirty());

    // Database should now be empty.
    EXPECT_TRUE(store.configs(drive)->empty());

    // Config should no longer be accessible.
    EXPECT_EQ(store.getByBackupId(configBefore.mBackupId), nullptr);
    EXPECT_EQ(store.getByRootHandle(configBefore.mRemoteNode), nullptr);
}

TEST_F(JSONSyncConfigStoreTest, UpdateRemoveTargetHandle)
{
    // Make sure database is removed.
    Directory drive(fsAccess(), Utilities::randomPath());

    // Create store.
    FakeNiceMock<ConfigStore> store(ioContext());

    // Create database.
    EXPECT_NE(store.create(drive), nullptr);
    EXPECT_NE(store.configs(drive), nullptr);
    EXPECT_TRUE(store.opened(drive));

    // Create config to add to database.
    SyncConfig configBefore;

    configBefore.mExternalDrivePath = drive;
    configBefore.mBackupId = 1;
    configBefore.mRemoteNode = 2;

    // Add config to database.
    const auto* c = store.add(configBefore);

    ASSERT_NE(c, nullptr);
    EXPECT_EQ(*c, configBefore);

    // Verify config has been added to database.
    EXPECT_EQ(store.getByBackupId(configBefore.mBackupId), c);
    EXPECT_EQ(store.getByRootHandle(configBefore.mRemoteNode), c);

    // Make sure database is clean.
    EXPECT_EQ(store.flush(), API_OK);

    // Update config.
    SyncConfig configAfter = configBefore;

    configAfter.mRemoteNode = UNDEF;

    // onChange should be generated when a config changes.
    Expectation onChange =
      EXPECT_CALL(store,
                  onChange(DB(drive.path()),
                           Eq(configBefore),
                           Eq(configAfter)))
        .Times(1);

    // onDirty should be generated when the database changes.
    EXPECT_CALL(store,
                onDirty(DB(drive.path())))
      .Times(1)
      .After(onChange);

    // Update the config.
    EXPECT_EQ(store.add(configAfter), c);

    // Verify config has been updated.
    EXPECT_EQ(*c, configAfter);

    // Database should be soiled.
    EXPECT_TRUE(store.dirty());

    // Is the config still accessible by tag?
    EXPECT_EQ(store.getByBackupId(configAfter.mBackupId), c);

    // Config should no longer be accessible by old target handle.
    EXPECT_EQ(store.getByRootHandle(configBefore.mRemoteNode), nullptr);

    // UNDEF should never be a valid mapping.
    EXPECT_EQ(store.getByRootHandle(UNDEF), nullptr);
}

} // JSONSyncConfigTests

#endif

