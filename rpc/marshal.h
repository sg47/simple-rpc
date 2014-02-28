#pragma once

#include <list>
#include <vector>
#include <string>
#include <map>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <limits>

#include <inttypes.h>
#include <string.h>
#include <unistd.h>

#include "utils.h"

namespace rpc {

// not thread safe, for better performance
class Marshal: public NoCopy {
    struct raw_bytes: public RefCounted {
        char* ptr;
        size_t size;
        static const size_t min_size;

        raw_bytes(size_t sz = min_size) {
            size = std::max(sz, min_size);
            ptr = new char[size];
        }
        raw_bytes(const void* p, size_t n) {
            size = std::max(n, min_size);
            ptr = new char[size];
            memcpy(ptr, p, n);
        }
        ~raw_bytes() { delete[] ptr; }
    };

    struct chunk: public NoCopy {
        raw_bytes* data;
        size_t read_idx;
        size_t write_idx;
        bool rdonly;
        chunk* next;

        chunk(): data(new raw_bytes), read_idx(0), write_idx(0), rdonly(false), next(nullptr) {}
        chunk(const void* p, size_t n): data(new raw_bytes(p, n)), read_idx(0), write_idx(n), rdonly(false), next(nullptr) {}
        ~chunk() { data->release(); }

    private:
        // make readonly copy
        chunk(raw_bytes* data, size_t read_idx, size_t write_idx)
                : data((raw_bytes *) data->ref_copy()), read_idx(read_idx), write_idx(write_idx), rdonly(true), next(nullptr) {
            assert(write_idx <= data->size);
            assert(read_idx <= write_idx);
        }
    public:
        chunk* rdonly_copy() const {
            return new chunk(data, read_idx, write_idx);
        }

        size_t content_size() const {
            assert(write_idx <= data->size);
            assert(read_idx <= write_idx);
            return write_idx - read_idx;
        }

        char* set_bookmark() {
            assert(write_idx <= data->size);
            assert(read_idx <= write_idx);

            verify(!rdonly);
            char* p = &data->ptr[write_idx];
            write_idx++;

            assert(write_idx <= data->size);
            assert(read_idx <= write_idx);
            return p;
        }

        size_t write(const void* p, size_t n) {
            assert(write_idx <= data->size);
            assert(read_idx <= write_idx);

            verify(!rdonly);
            size_t n_write = std::min(n, data->size - write_idx);
            if (n_write > 0) {
                memcpy(data->ptr + write_idx, p, n_write);
            }
            write_idx += n_write;

            assert(write_idx <= data->size);
            assert(read_idx <= write_idx);
            return n_write;
        }

        size_t read(void* p, size_t n) {
            assert(write_idx <= data->size);
            assert(read_idx <= write_idx);

            size_t n_read = std::min(n, write_idx - read_idx);
            if (n_read > 0) {
                memcpy(p, data->ptr + read_idx, n_read);
            }
            read_idx += n_read;

            assert(write_idx <= data->size);
            assert(read_idx <= write_idx);
            return n_read;
        }

        size_t peek(void* p, size_t n) const {
            assert(write_idx <= data->size);
            assert(read_idx <= write_idx);

            size_t n_peek = std::min(n, write_idx - read_idx);
            if (n_peek > 0) {
                memcpy(p, data->ptr + read_idx, n_peek);
            }

            return n_peek;
        }

        size_t discard(size_t n) {
            assert(write_idx <= data->size);
            assert(read_idx <= write_idx);

            size_t n_discard = std::min(n, write_idx - read_idx);
            read_idx += n_discard;

            assert(write_idx <= data->size);
            assert(read_idx <= write_idx);
            return n_discard;
        }

        int write_to_fd(int fd) {
            assert(write_idx <= data->size);
            int cnt = ::write(fd, data->ptr + read_idx, write_idx - read_idx);
            if (cnt > 0) {
                read_idx += cnt;
            }

            assert(write_idx <= data->size);
            return cnt;
        }

        int read_from_fd(int fd) {
            assert(write_idx <= data->size);
            assert(read_idx <= write_idx);

            verify(!rdonly);
            int cnt = 0;
            if (write_idx < data->size) {
                cnt = ::read(fd, data->ptr + write_idx, data->size - write_idx);
                if (cnt > 0) {
                    write_idx += cnt;
                }
            }

            assert(write_idx <= data->size);
            assert(read_idx <= write_idx);
            return cnt;
        }

        // check if it is not possible to write to the chunk anymore.
        bool fully_written() const {
            assert(write_idx <= data->size);
            assert(read_idx <= write_idx);
            return write_idx == data->size;
        }

        // check if it is not possible to read any data even if retry later
        bool fully_read() const {
            assert(write_idx <= data->size);
            assert(read_idx <= write_idx);
            return read_idx == data->size;
        }
    };

public:

    struct bookmark: public NoCopy {
        size_t size;
        char** ptr;

        ~bookmark() {
            delete[] ptr;
        }
    };

private:

    chunk* head_;
    chunk* tail_;
    i32 write_cnt_;
    size_t content_size_;

    // for debugging purpose
    size_t content_size_slow() const;

public:

    Marshal(): head_(nullptr), tail_(nullptr), write_cnt_(0), content_size_(0) { }
    ~Marshal();

    bool empty() const {
        assert(content_size_ == content_size_slow());
        return content_size_ == 0;
    }
    size_t content_size() const {
        assert(content_size_ == content_size_slow());
        return content_size_;
    }

    size_t write(const void* p, size_t n);
    size_t read(void* p, size_t n);
    size_t peek(void* p, size_t n) const;

    size_t read_from_fd(int fd);

    // NOTE: this function is only used *internally* to chop a slice of marshal object
    size_t read_from_marshal(Marshal& m, size_t n);

    size_t write_to_fd(int fd);

    bookmark* set_bookmark(size_t n);
    void write_bookmark(bookmark* bm, const void* p) {
        const char* pc = (const char *) p;
        assert(bm != nullptr && bm->ptr != nullptr && p != nullptr);
        for (size_t i = 0; i < bm->size; i++) {
            *(bm->ptr[i]) = pc[i];
        }
    }

    i32 get_and_reset_write_cnt() {
        i32 cnt = write_cnt_;
        write_cnt_ = 0;
        return cnt;
    }
};

inline rpc::Marshal& operator <<(rpc::Marshal& m, const rpc::i8& v) {
    verify(m.write(&v, sizeof(v)) == sizeof(v));
    return m;
}

inline rpc::Marshal& operator <<(rpc::Marshal& m, const rpc::i16& v) {
    verify(m.write(&v, sizeof(v)) == sizeof(v));
    return m;
}

inline rpc::Marshal& operator <<(rpc::Marshal& m, const rpc::i32& v) {
    verify(m.write(&v, sizeof(v)) == sizeof(v));
    return m;
}

inline rpc::Marshal& operator <<(rpc::Marshal& m, const rpc::i64& v) {
    verify(m.write(&v, sizeof(v)) == sizeof(v));
    return m;
}

inline rpc::Marshal& operator <<(rpc::Marshal& m, const rpc::v32& v) {
    char buf[5];
    size_t bsize = base::SparseInt::dump(v.get(), buf);
    verify(m.write(buf, bsize) == bsize);
    return m;
}

inline rpc::Marshal& operator <<(rpc::Marshal& m, const rpc::v64& v) {
    char buf[9];
    size_t bsize = base::SparseInt::dump(v.get(), buf);
    verify(m.write(buf, bsize) == bsize);
    return m;
}

inline rpc::Marshal& operator <<(rpc::Marshal& m, const double& v) {
    verify(m.write(&v, sizeof(v)) == sizeof(v));
    return m;
}

inline rpc::Marshal& operator <<(rpc::Marshal& m, const std::string& v) {
    v64 v_len = v.length();
    m << v_len;
    if (v_len.get() > 0) {
        verify(m.write(v.c_str(), v_len.get()) == (size_t) v_len.get());
    }
    return m;
}

template<class T1, class T2>
inline rpc::Marshal& operator <<(rpc::Marshal& m, const std::pair<T1, T2>& v) {
    m << v.first;
    m << v.second;
    return m;
}

template<class T>
inline rpc::Marshal& operator <<(rpc::Marshal& m, const std::vector<T>& v) {
    v64 v_len = v.size();
    m << v_len;
    for (typename std::vector<T>::const_iterator it = v.begin(); it != v.end(); ++it) {
        m << *it;
    }
    return m;
}

template<class T>
inline rpc::Marshal& operator <<(rpc::Marshal& m, const std::list<T>& v) {
    v64 v_len = v.size();
    m << v_len;
    for (typename std::list<T>::const_iterator it = v.begin(); it != v.end(); ++it) {
        m << *it;
    }
    return m;
}

template<class T>
inline rpc::Marshal& operator <<(rpc::Marshal& m, const std::set<T>& v) {
    v64 v_len = v.size();
    m << v_len;
    for (typename std::set<T>::const_iterator it = v.begin(); it != v.end(); ++it) {
        m << *it;
    }
    return m;
}

template<class K, class V>
inline rpc::Marshal& operator <<(rpc::Marshal& m, const std::map<K, V>& v) {
    v64 v_len = v.size();
    m << v_len;
    for (typename std::map<K, V>::const_iterator it = v.begin(); it != v.end(); ++it) {
        m << it->first << it->second;
    }
    return m;
}

template<class T>
inline rpc::Marshal& operator <<(rpc::Marshal& m, const std::unordered_set<T>& v) {
    v64 v_len = v.size();
    m << v_len;
    for (typename std::unordered_set<T>::const_iterator it = v.begin(); it != v.end(); ++it) {
        m << *it;
    }
    return m;
}

template<class K, class V>
inline rpc::Marshal& operator <<(rpc::Marshal& m, const std::unordered_map<K, V>& v) {
    v64 v_len = v.size();
    m << v_len;
    for (typename std::unordered_map<K, V>::const_iterator it = v.begin(); it != v.end(); ++it) {
        m << it->first << it->second;
    }
    return m;
}

inline rpc::Marshal& operator >>(rpc::Marshal& m, rpc::i8& v) {
    verify(m.read(&v, sizeof(v)) == sizeof(v));
    return m;
}

inline rpc::Marshal& operator >>(rpc::Marshal& m, rpc::i16& v) {
    verify(m.read(&v, sizeof(v)) == sizeof(v));
    return m;
}

inline rpc::Marshal& operator >>(rpc::Marshal& m, rpc::i32& v) {
    verify(m.read(&v, sizeof(v)) == sizeof(v));
    return m;
}

inline rpc::Marshal& operator >>(rpc::Marshal& m, rpc::i64& v) {
    verify(m.read(&v, sizeof(v)) == sizeof(v));
    return m;
}

inline rpc::Marshal& operator >>(rpc::Marshal& m, rpc::v32& v) {
    char byte0;
    verify(m.peek(&byte0, 1) == 1);
    size_t bsize = base::SparseInt::buf_size(byte0);
    char buf[5];
    verify(m.read(buf, bsize) == bsize);
    i32 val = base::SparseInt::load_i32(buf);
    v.set(val);
    return m;
}

inline rpc::Marshal& operator >>(rpc::Marshal& m, rpc::v64& v) {
    char byte0;
    verify(m.peek(&byte0, 1) == 1);
    size_t bsize = base::SparseInt::buf_size(byte0);
    char buf[9];
    verify(m.read(buf, bsize) == bsize);
    i64 val = base::SparseInt::load_i64(buf);
    v.set(val);
    return m;
}

inline rpc::Marshal& operator >>(rpc::Marshal& m, double& v) {
    verify(m.read(&v, sizeof(v)) == sizeof(v));
    return m;
}

inline rpc::Marshal& operator >>(rpc::Marshal& m, std::string& v) {
    v64 v_len;
    m >> v_len;
    v.resize(v_len.get());
    if (v_len.get() > 0) {
        verify(m.read(&v[0], v_len.get()) == (size_t) v_len.get());
    }
    return m;
}

template<class T1, class T2>
inline rpc::Marshal& operator >>(rpc::Marshal& m, std::pair<T1, T2>& v) {
    m >> v.first;
    m >> v.second;
    return m;
}

template<class T>
inline rpc::Marshal& operator >>(rpc::Marshal& m, std::vector<T>& v) {
    v64 v_len;
    m >> v_len;
    v.clear();
    v.reserve(v_len.get());
    for (int i = 0; i < v_len.get(); i++) {
        T elem;
        m >> elem;
        v.push_back(elem);
    }
    return m;
}

template<class T>
inline rpc::Marshal& operator >>(rpc::Marshal& m, std::list<T>& v) {
    v64 v_len;
    m >> v_len;
    v.clear();
    for (int i = 0; i < v_len.get(); i++) {
        T elem;
        m >> elem;
        v.push_back(elem);
    }
    return m;
}

template<class T>
inline rpc::Marshal& operator >>(rpc::Marshal& m, std::set<T>& v) {
    v64 v_len;
    m >> v_len;
    v.clear();
    for (int i = 0; i < v_len.get(); i++) {
        T elem;
        m >> elem;
        v.insert(elem);
    }
    return m;
}

template<class K, class V>
inline rpc::Marshal& operator >>(rpc::Marshal& m, std::map<K, V>& v) {
    v64 v_len;
    m >> v_len;
    v.clear();
    for (int i = 0; i < v_len.get(); i++) {
        K key;
        V value;
        m >> key >> value;
        insert_into_map(v, key, value);
    }
    return m;
}

template<class T>
inline rpc::Marshal& operator >>(rpc::Marshal& m, std::unordered_set<T>& v) {
    v64 v_len;
    m >> v_len;
    v.clear();
    for (int i = 0; i < v_len.get(); i++) {
        T elem;
        m >> elem;
        v.insert(elem);
    }
    return m;
}

template<class K, class V>
inline rpc::Marshal& operator >>(rpc::Marshal& m, std::unordered_map<K, V>& v) {
    v64 v_len;
    m >> v_len;
    v.clear();
    for (int i = 0; i < v_len.get(); i++) {
        K key;
        V value;
        m >> key >> value;
        insert_into_map(v, key, value);
    }
    return m;
}

} // namespace rpc
