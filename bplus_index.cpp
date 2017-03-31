#include "bplus_index.h"
#include "util.h"
using namespace std;
BPLUSIndex::BPLUSIndex(uint64_t num_ents) {
    num_ents_ = num_ents;
    cout << "NUM ENTS: " << to_string(num_ents) << endl;
    // generate path
    string name = "/home/arvin/FileSystem/zipfs/o/dir/root/TREE-"+ Util::generate_rand_hex_name();
    // initialize memory zone
    mem_ = TireFire(name);
    inodes_idx_ = mem_.get_tire(sizeof(inode) * num_ents_);
}

void BPLUSIndex::add_inode(Inode in, map<uint64_t, shared_ptr<Block>> dirty_blocks,
                           map<uint64_t, unsigned long long> block_mtime) {
    (void)dirty_blocks;
    (void)block_mtime;
    cout << "INSERTING " << in.get_id() << endl;
    bool isroot = false;
    if (root_ptr_ == NULL) {
        rootidx_ = mem_.get_tire(sizeof(node));
        root_ptr_ = (node*)mem_.get_memory(rootidx_);
        cout << "ROOT OFFSET: " << to_string(mem_.get_offset(rootidx_));
        cout << "SIZEOF NODE: " << to_string(sizeof(node)) << endl;
        root_ptr_->is_leaf = 1;
        root_ptr_->num_keys = 1;
        cur_inode_arr_idx_ = 0;
        root_ptr_->parent = -1;
        // allocate memory for all inodes
        inode_arr_idx_ = mem_.get_tire(sizeof(inode) * num_ents_);
        isroot = true;

    }

    // construct inode
    // construct key
    int64_t key_idx = mem_.get_tire(256 * sizeof(char));
    char* key = (char*)mem_.get_memory(key_idx);
    memset(key, '\0', 256 * sizeof(char));
    memcpy(key, in.get_id().c_str(), in.get_id().size());
    cout << "MADE KEY! " << key << endl;
    // insert this inode
    inode* cur_inode_ptr = (inode*)mem_.get_memory(inode_arr_idx_) + cur_inode_arr_idx_;
    cur_inode_ptr->mode = in.get_mode();
    cur_inode_ptr->nlink = in.get_link();
    cur_inode_ptr->mtime = in.get_ull_mtime();
    cur_inode_ptr->ctime = in.get_ull_ctime();
    cur_inode_ptr->size = in.get_size();
    cur_inode_ptr->deleted = in.is_deleted();
    cur_inode_ptr->hash = mem_.get_offset(key_idx);
    // initialize blocks
    uint64_t bd_size = sizeof(block_data) *  dirty_blocks.size();
    // construct blocks dirty blocks for inode
    int64_t blocksidx = mem_.get_tire(bd_size);


    for (auto db : dirty_blocks) {
        block_data* loblocks = (block_data*)mem_.get_memory(blocksidx);
        block_data* cur = loblocks + db.first;
        cur->block_id = db.first;
        cur->size = db.second->get_actual_size();

        // construct data in memory
        int64_t bdataidx = mem_.get_tire(cur->size * sizeof(uint8_t));
        uint8_t* bdata = (uint8_t*)mem_.get_memory(bdataidx);
        auto bytes = db.second->get_data();
        loblocks = (block_data*)mem_.get_memory(blocksidx);
        cur = loblocks + db.first;

        for (uint32_t ii = 0; ii < cur->size; ii++) {
            bdata[ii] = bytes[ii];

        }
        cur->data_offset = mem_.get_offset(bdataidx);
        cur->mtime = block_mtime[db.first];
    }

    cur_inode_ptr = (inode*)mem_.get_memory(inode_arr_idx_) + cur_inode_arr_idx_;
    if (bd_size > 0) {
        cur_inode_ptr->block_data = mem_.get_offset(blocksidx);
        cur_inode_ptr->block_data_size = bd_size;
    } else
        cur_inode_ptr->block_data = -1;

    // compute offsets, inc inode array idx
    uint64_t inode_of = (cur_inode_arr_idx_ * sizeof(inode)) + mem_.get_offset(inodes_idx_);
    uint64_t key_of = mem_.get_offset(key_idx);
    cur_inode_arr_idx_++;

    // insert into node w/ blocks info
    if (isroot) {
        root_ptr_ = (node*)mem_.get_memory(rootidx_);
        root_ptr_->keys[0] = key_of;
        root_ptr_->values[0] = inode_of;
        return;
    }

    // else find a node to insert it
    uint64_t target_offset = find_node_to_store(in.get_id());
    // check if the node is full
    node* tnode = (node*)((char*)mem_.get_root() + target_offset);
    if (tnode->num_keys == ORDER - 1) {
        // needs to split the node to fit this item
        split_insert_node(target_offset, key_of, inode_of, false, -1);
        return;
    }
    // or else we can just insert it
    insert_into_node(target_offset, key_of, inode_of, false, -1);

}

int64_t
BPLUSIndex::find_node_to_store(string key) {
    if (root_ptr_ == nullptr)
        throw domain_error("tree is empty");

    node* cur = nullptr;
    int64_t cur_offset = mem_.get_offset(rootidx_);
    bool change = false;
    for (;;) {
        if (!change)
            cur = (node*)mem_.get_memory(rootidx_);
        else
            cur = (node*)((char*)mem_.get_root() + cur_offset);
        if (cur->is_leaf)
            return cur_offset;

        // is this key before all of the ones in this node?
        char* cur_key = (char*)mem_.get_root() + cur->keys[0];
        if (strcmp(key.c_str(), cur_key) < 0) {
            cur_offset = before(cur, 0);
            change = true;
            continue;
        }

        // is this key after the ones in this node?
        cur_key = (char*)mem_.get_root() + cur->keys[cur->num_keys - 1];
        if (strcmp(key.c_str(), cur_key) >= 0) {
            cur_offset = after(cur, cur->num_keys - 1);
            change = true;
            continue;
        }

        // else it must be somewhere in between
        uint64_t inneridx = 0;
        for (int ii = 1; ii < cur->num_keys - 1; ii++) {
            char* prev = (char*)mem_.get_root() + cur->keys[ii];
            char* post = (char*)mem_.get_root() + cur->keys[ii + 1];
            if (strcmp(prev, key.c_str()) <= 0
                    && strcmp(key.c_str(), post) < 0)
                inneridx = ii;
        }
        cur_offset = after(cur, inneridx);
        change = true;
    }
}

int64_t
BPLUSIndex::before(node* n, int64_t idx) {
    return n->children[idx];
}

int64_t
BPLUSIndex::after(node* n, int64_t idx) {
    return n->children[idx];
}
int
BPLUSIndex::insert_into_node(uint64_t nodeoffset, int64_t k, int64_t v, bool isleft, int64_t child) {
    // find spot
    int cur_idx = 0;
    node * n = (node*)((char*)mem_.get_root() + nodeoffset);
    cout << "NUM KEYS: " << to_string(n->num_keys) << endl;
    for (int ii = 0; ii < n->num_keys - 1; ii++) {
        // compare keys by string
        // TODO:
        cout << "Offset: " << to_string(n->keys[ii]) << endl;
        char* oldk = (char*)mem_.get_root() + n->keys[ii];
        char* newk = (char*)mem_.get_root() + k;
        cout << "oldk: " << oldk << endl;
        cout << "newk: " << newk << endl;
        if (strcmp(oldk, newk) > 0)
            cur_idx = ii;
    }

    if (cur_idx == 0 && n->num_keys > 0)
        cur_idx = n->num_keys;
    else {
        // move everything from cur_idx + 1 up
        node temp = *n;
        for (int ii = cur_idx; ii < n->num_keys - 1; ii++) {
            n->keys[ii + 1] = temp.keys[ii];
            n->children[ii + 1] = temp.children[ii];
            n->values[ii + 1] = temp.values[ii];

        }
    }
    cout << "PUTTING K IN " << to_string(cur_idx) << endl;
    n->keys[cur_idx] = k;
    n->num_keys++;

    if (n->is_leaf) {
        // insert value into fixed node
        n->values[cur_idx] = v;
        n->children[cur_idx] = 0;

    } else {
        // insert child
        if (!isleft)
            cur_idx++;
        n->children[cur_idx] = child;
    }
    cout << "NEW NUM KEYS: " << to_string(n->num_keys);
    return cur_idx;
}

int64_t
BPLUSIndex::split_insert_node(uint64_t nodeoffset, int64_t k, int64_t v, bool isparent, int64_t targ) {
    cout << "SPLITTING" << endl;
    // get memory for node
    node* nnode = (node*)((char*)mem_.get_root() + nodeoffset);

    // do nothing if the node is not full
    if (nnode->num_keys != ORDER - 1)
        return nodeoffset;

    // get median key index
    uint64_t med_idx = ((ORDER - 1) / 2);

    // make new leaves
    int64_t rightidx = mem_.get_tire(sizeof(node));
    node* right = (node*)mem_.get_memory(rightidx);
    right->is_leaf = true;
    // copy stuff from median to end over to new node
    uint64_t ii;
    uint64_t cur_idx = 0;
    // refresh pointer
    nnode = (node*)((char*)mem_.get_root() + nodeoffset);
    int64_t med_key_idx = nnode->keys[med_idx];
    // if we are splitting a parent, don't include median on right
    if (isparent) {
        cout << "MED IDX" << to_string(med_idx) << endl;
        for (ii = med_idx + 1; ii < ORDER - 1; ii++, cur_idx++) {
            right->num_keys++;
            right->keys[cur_idx] = nnode->keys[ii];
            right->children[cur_idx] = nnode->children[ii];
            // delete now redundant entries
            nnode->keys[ii] = -1;
            nnode->num_keys--;
        }
        // remove med from left
        nnode->keys[med_idx] = -1;
        nnode->num_keys--;
        // right side is also a parent, insert the target (carried up) node
        right->is_leaf = false;
        cout << "Split insert" << endl;
        insert_into_node(mem_.get_offset(rightidx), k, v, false, mem_.get_offset(targ));
        // set parent pointer in targ
        node* target = (node*)mem_.get_memory(targ);
        target->parent = mem_.get_offset(rightidx);

    } else {
        // we are splitting a child
        for (ii = med_idx; ii < ORDER - 1; ii++, cur_idx++) {
            right->num_keys++;
            right->keys[cur_idx] = nnode->keys[ii];
            right->children[cur_idx] = nnode->children[ii];
            // copy values over
            right->values[cur_idx] = nnode->values[ii];

            // delete redundant entries
            nnode->keys[ii] = -1;
        }
        nnode->num_keys = med_idx;

        // now that the leaf is split, insert into new side
        if (v != -1)
            insert_into_node(mem_.get_offset(rightidx), k, v, false, -1);
    }
    // fix parent pointers
    int64_t paridx = nnode->parent;

    int64_t nkey = right->keys[0];
    if (isparent)
        nkey = med_key_idx;
    // make new root if parent is null
    if (paridx == -1) {
        // make new root
        int64_t nrootidx = mem_.get_tire(sizeof(node));
        node* nroot = (node*)mem_.get_memory(nrootidx);
        nroot->num_keys = 1;
        nroot->is_leaf = false;
        nroot->keys[0] = nkey;
        nroot->children[0] = nodeoffset;
        nroot->children[1] = mem_.get_offset(rightidx);
        right = (node*)mem_.get_memory(rightidx);
        right->parent = nrootidx;
        nnode = (node*)((char*)mem_.get_root() + nodeoffset);
        nnode->parent = nrootidx;
        rootidx_ = nrootidx;
        return nrootidx;
    }
    // parent exists
    node* pparent = (node*)mem_.get_memory(paridx);
    right = (node*)mem_.get_memory(rightidx);

    if (pparent->num_keys == ORDER - 1) {
        // no room, need to split
        paridx = split_insert_node(mem_.get_offset(paridx), right->keys[0], -1, true, rightidx);
    } else {
        insert_into_node(mem_.get_offset(paridx), nkey, v, false, mem_.get_offset(rightidx));
        right->parent = paridx;
    }
    return -1;
}

BPLUSIndex::~BPLUSIndex() {
    mem_.end();
}
