# PES-VCS — Version Control System

## 👤 Author

* **Name:** Vikas N
* **SRN:** PES2UG24CS668

---

## 📌 Project Overview

This project implements a simplified Git-like Version Control System using content-addressable storage, trees, index, and commits.

---

## ⚙️ Build Instructions

```bash
make
make test_objects
make test_tree
make test-integration
```

---

# 🔹 Phase 1 — Object Storage

## ✅ Output

![Phase 1 Output](screenshots/a1.png)

## 📁 Object Storage Structure

![Objects](screenshots/1b.png)

### 🧠 Explanation

* SHA-256 based storage
* Deduplication
* Integrity check

---

# 🔹 Phase 2 — Tree Objects

## ✅ Test Output

![Phase 2 Output](screenshots/2a.png)

### 🧠 Explanation

* Tree = directory structure
* Deterministic serialization

---

# 🔹 Phase 3 — Index (Staging Area)

## ✅ Status Output

![Index Status](screenshots/3a.png)

## 📄 Index File

![Index File](screenshots/3B.png)

### 🧠 Explanation

* Stores staged files
* Maintains metadata

---

# 🔹 Phase 4 — Commits

## ✅ Commit Log

![Commit Log](screenshots/4A.png)

## 🔗 HEAD and References

![HEAD](screenshots/4B.png)
![Reference](screenshots/4C.png)


### 🧠 Explanation

* Commits store snapshots
* Linked via parent hash

---

# 🔹 Integration Test

## ✅ Output

![Integration](screenshots/alltest1.png)
![Integration](screenshots/alltest2.png)
![Integration](screenshots/alltest3.png)

### 🧠 Explanation

* Full workflow tested successfully

---

# 📁 Files Implemented

* object.c
* tree.c
* index.c
* commit.c
* pes.c

---

# 🎯 Final Status

* ✔ All phases completed
* ✔ All tests passed
* ✔ Integration successful

---

# 📌 Conclusion

This project demonstrates the internal working of Git-like systems using hashing, trees, and commits.
