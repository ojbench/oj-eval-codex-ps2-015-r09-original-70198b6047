#include <bits/stdc++.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

using namespace std;

namespace {

constexpr uint32_t kBucketCount = 1u << 17;
constexpr const char* kDbFile = "storage.db";
constexpr uint32_t kMagic = 0x31353044u; // D051

#pragma pack(push, 1)
struct Header {
  uint32_t magic;
  uint32_t bucket_count;
  uint64_t next_offset;
};

struct KeyNode {
  uint32_t hash;
  uint32_t key_len;
  uint64_t next;
  uint64_t root;
  char key[64];
};

struct ValueNode {
  int32_t value;
  uint32_t priority;
  uint64_t left;
  uint64_t right;
};
#pragma pack(pop)

static_assert(sizeof(Header) == 16);
static_assert(sizeof(KeyNode) == 88);
static_assert(sizeof(ValueNode) == 24);

class FastInput {
 public:
  FastInput() { refill(); }

  bool next_token(string& out) {
    out.clear();
    char ch;
    do {
      if (!read_char(ch)) return false;
    } while (isspace(static_cast<unsigned char>(ch)));
    do {
      out.push_back(ch);
      if (!read_char(ch)) return true;
    } while (!isspace(static_cast<unsigned char>(ch)));
    return true;
  }

  bool next_int(int& out) {
    string token;
    if (!next_token(token)) return false;
    out = stoi(token);
    return true;
  }

 private:
  static constexpr size_t kBufSize = 1 << 16;
  array<char, kBufSize> buf{};
  size_t pos = 0;
  size_t len = 0;

  void refill() {
    len = fread(buf.data(), 1, buf.size(), stdin);
    pos = 0;
  }

  bool read_char(char& ch) {
    if (pos >= len) {
      refill();
      if (len == 0) return false;
    }
    ch = buf[pos++];
    return true;
  }
};

class Database {
 public:
  Database() : buckets_(kBucketCount, 0) {
    open_db();
  }

  ~Database() {
    flush_header();
    if (fd_ >= 0) close(fd_);
  }

  void insert(const string& key, int value) {
    uint64_t key_offset = 0;
    KeyNode key_node{};
    bool found = locate_key(key, key_offset, key_node);
    if (!found) {
      key_offset = create_key_node(key);
      key_node = read_key_node(key_offset);
      const uint32_t bucket = key_node.hash & (kBucketCount - 1);
      key_node.next = buckets_[bucket];
      buckets_[bucket] = key_offset;
      write_bucket_if_needed(bucket);
    }
    key_node.root = insert_value(key_node.root, value);
    write_key_node(key_offset, key_node);
  }

  void erase(const string& key, int value) {
    uint64_t key_offset = 0;
    KeyNode key_node{};
    bool found = locate_key(key, key_offset, key_node);
    if (!found) return;
    uint64_t new_root = erase_value(key_node.root, value);
    if (new_root == key_node.root) return;
    key_node.root = new_root;
    write_key_node(key_offset, key_node);
  }

  void find(const string& key, ostream& out) {
    uint64_t key_offset = 0;
    KeyNode key_node{};
    bool found = locate_key(key, key_offset, key_node);
    if (!found || key_node.root == 0) {
      out << "null\n";
      return;
    }
    bool first = true;
    inorder_print(key_node.root, first, out);
    out << '\n';
  }

 private:
  int fd_ = -1;
  Header header_{};
  vector<uint64_t> buckets_;
  mt19937_64 rng_{random_device{}()};

  static uint32_t hash_key(const string& key) {
    uint64_t h = 1469598103934665603ull;
    for (unsigned char c : key) {
      h ^= c;
      h *= 1099511628211ull;
    }
    return static_cast<uint32_t>(h ^ (h >> 32));
  }

  void open_db() {
    fd_ = ::open(kDbFile, O_RDWR | O_CREAT, 0644);
    if (fd_ < 0) {
      perror("open");
      exit(1);
    }
    struct stat st {};
    if (fstat(fd_, &st) != 0) {
      perror("fstat");
      exit(1);
    }
    if (st.st_size < static_cast<off_t>(sizeof(Header) + sizeof(uint64_t) * kBucketCount)) {
      header_.magic = kMagic;
      header_.bucket_count = kBucketCount;
      header_.next_offset = sizeof(Header) + sizeof(uint64_t) * kBucketCount;
      write_all(0, &header_, sizeof(header_));
      write_all(sizeof(Header), buckets_.data(), buckets_.size() * sizeof(uint64_t));
      return;
    }
    read_all(0, &header_, sizeof(header_));
    if (header_.magic != kMagic || header_.bucket_count != kBucketCount) {
      cerr << "Invalid database file" << endl;
      exit(1);
    }
    read_all(sizeof(Header), buckets_.data(), buckets_.size() * sizeof(uint64_t));
  }

  void flush_header() {
    if (fd_ < 0) return;
    write_all(0, &header_, sizeof(header_));
    write_all(sizeof(Header), buckets_.data(), buckets_.size() * sizeof(uint64_t));
  }

  void write_bucket_if_needed(size_t index) {
    off_t off = sizeof(Header) + static_cast<off_t>(index) * sizeof(uint64_t);
    write_all(off, &buckets_[index], sizeof(uint64_t));
  }

  void* write_all(off_t off, const void* data, size_t size) {
    const char* ptr = static_cast<const char*>(data);
    size_t done = 0;
    while (done < size) {
      ssize_t w = pwrite(fd_, ptr + done, size - done, off + static_cast<off_t>(done));
      if (w < 0) {
        if (errno == EINTR) continue;
        perror("pwrite");
        exit(1);
      }
      done += static_cast<size_t>(w);
    }
    if (off + static_cast<off_t>(size) > static_cast<off_t>(header_.next_offset)) {
      header_.next_offset = off + size;
    }
    return nullptr;
  }

  void read_all(off_t off, void* data, size_t size) {
    char* ptr = static_cast<char*>(data);
    size_t done = 0;
    while (done < size) {
      ssize_t r = pread(fd_, ptr + done, size - done, off + static_cast<off_t>(done));
      if (r < 0) {
        if (errno == EINTR) continue;
        perror("pread");
        exit(1);
      }
      if (r == 0) {
        memset(ptr + done, 0, size - done);
        return;
      }
      done += static_cast<size_t>(r);
    }
  }

  uint64_t allocate(size_t size) {
    uint64_t off = header_.next_offset;
    header_.next_offset += size;
    return off;
  }

  KeyNode read_key_node(uint64_t off) {
    KeyNode node{};
    read_all(static_cast<off_t>(off), &node, sizeof(node));
    return node;
  }

  void write_key_node(uint64_t off, const KeyNode& node) {
    write_all(static_cast<off_t>(off), &node, sizeof(node));
  }

  ValueNode read_value_node(uint64_t off) {
    ValueNode node{};
    read_all(static_cast<off_t>(off), &node, sizeof(node));
    return node;
  }

  void write_value_node(uint64_t off, const ValueNode& node) {
    write_all(static_cast<off_t>(off), &node, sizeof(node));
  }

  uint64_t create_key_node(const string& key) {
    KeyNode node{};
    node.hash = hash_key(key);
    node.key_len = static_cast<uint32_t>(key.size());
    memcpy(node.key, key.data(), key.size());
    node.next = 0;
    node.root = 0;
    uint64_t off = allocate(sizeof(KeyNode));
    write_key_node(off, node);
    return off;
  }

  bool locate_key(const string& key, uint64_t& found_offset, KeyNode& found_node) {
    uint32_t h = hash_key(key);
    size_t bucket = h & (kBucketCount - 1);
    uint64_t cur = buckets_[bucket];
    while (cur != 0) {
      KeyNode node = read_key_node(cur);
      if (node.hash == h && node.key_len == key.size() && memcmp(node.key, key.data(), key.size()) == 0) {
        found_offset = cur;
        found_node = node;
        return true;
      }
      cur = node.next;
    }
    found_offset = 0;
    found_node = KeyNode{};
    return false;
  }

  uint64_t new_value_node(int value) {
    ValueNode node{};
    node.value = value;
    node.priority = static_cast<uint32_t>(rng_());
    node.left = 0;
    node.right = 0;
    uint64_t off = allocate(sizeof(ValueNode));
    write_value_node(off, node);
    return off;
  }

  uint64_t rotate_right(uint64_t root_off) {
    ValueNode root = read_value_node(root_off);
    uint64_t left_off = root.left;
    ValueNode left = read_value_node(left_off);
    root.left = left.right;
    left.right = root_off;
    write_value_node(root_off, root);
    write_value_node(left_off, left);
    return left_off;
  }

  uint64_t rotate_left(uint64_t root_off) {
    ValueNode root = read_value_node(root_off);
    uint64_t right_off = root.right;
    ValueNode right = read_value_node(right_off);
    root.right = right.left;
    right.left = root_off;
    write_value_node(root_off, root);
    write_value_node(right_off, right);
    return right_off;
  }

  uint64_t insert_value(uint64_t root_off, int value) {
    if (root_off == 0) return new_value_node(value);
    ValueNode root = read_value_node(root_off);
    if (value == root.value) return root_off;
    if (value < root.value) {
      uint64_t new_left = insert_value(root.left, value);
      root.left = new_left;
      write_value_node(root_off, root);
      if (new_left != 0) {
        ValueNode left = read_value_node(new_left);
        if (left.priority < root.priority) {
          return rotate_right(root_off);
        }
      }
    } else {
      uint64_t new_right = insert_value(root.right, value);
      root.right = new_right;
      write_value_node(root_off, root);
      if (new_right != 0) {
        ValueNode right = read_value_node(new_right);
        if (right.priority < root.priority) {
          return rotate_left(root_off);
        }
      }
    }
    return root_off;
  }

  uint64_t merge(uint64_t left_off, uint64_t right_off) {
    if (left_off == 0) return right_off;
    if (right_off == 0) return left_off;
    ValueNode left = read_value_node(left_off);
    ValueNode right = read_value_node(right_off);
    if (left.priority < right.priority) {
      left.right = merge(left.right, right_off);
      write_value_node(left_off, left);
      return left_off;
    }
    right.left = merge(left_off, right.left);
    write_value_node(right_off, right);
    return right_off;
  }

  uint64_t erase_value(uint64_t root_off, int value) {
    if (root_off == 0) return 0;
    ValueNode root = read_value_node(root_off);
    if (value < root.value) {
      uint64_t new_left = erase_value(root.left, value);
      if (new_left == root.left) return root_off;
      root.left = new_left;
      write_value_node(root_off, root);
      return root_off;
    }
    if (value > root.value) {
      uint64_t new_right = erase_value(root.right, value);
      if (new_right == root.right) return root_off;
      root.right = new_right;
      write_value_node(root_off, root);
      return root_off;
    }
    return merge(root.left, root.right);
  }

  void inorder_print(uint64_t root_off, bool& first, ostream& out) {
    if (root_off == 0) return;
    ValueNode node = read_value_node(root_off);
    inorder_print(node.left, first, out);
    if (!first) out << ' ';
    first = false;
    out << node.value;
    inorder_print(node.right, first, out);
  }
};

}  // namespace

int main() {
  ios::sync_with_stdio(false);
  cin.tie(nullptr);

  FastInput input;
  int n = 0;
  if (!input.next_int(n)) return 0;
  Database db;
  for (int i = 0; i < n; ++i) {
    string cmd, key;
    input.next_token(cmd);
    input.next_token(key);
    if (cmd == "insert") {
      int value;
      input.next_int(value);
      db.insert(key, value);
    } else if (cmd == "delete") {
      int value;
      input.next_int(value);
      db.erase(key, value);
    } else if (cmd == "find") {
      db.find(key, cout);
    }
  }
  return 0;
}
