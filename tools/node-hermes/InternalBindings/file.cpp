/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "InternalBindings.h"
#include "hermes/hermes.h"

#include "uv.h"

using namespace facebook;

/// Given the directory that the original file being read is in and the
/// relative path of the target, forms the absolute path
/// for the target.
static void canonicalizePath(
    llvh::SmallVectorImpl<char> &dirname,
    llvh::StringRef target) {
  if (!target.empty() && target[0] == '/') {
    // If the target is absolute (starts with a '/'), resolve from the file
    // system root.
    dirname.clear();
    llvh::sys::path::append(dirname, llvh::sys::path::Style::posix, target);
    return;
  }
  llvh::sys::path::append(dirname, llvh::sys::path::Style::posix, target);

  // Remove all dots. This is done to get rid of ../ or anything like ././.
  llvh::sys::path::remove_dots(dirname, true, llvh::sys::path::Style::posix);
}

/// Takes a file path and returns a file descriptor representing the open file.
/// Called by js the following way:
/// fd = binding.open(path, flags, mode, FSReqCallback, ctx)
/// In the synchronous version, FSReqCallback will always be undefined.
/// Currently only the synchronous version is supported.
static jsi::Value open(RuntimeState &rs, const jsi::Value *args, size_t count) {
  jsi::Runtime &rt = rs.getRuntime();
  if (count < 5) {
    throw jsi::JSError(
        rt, "Not enough arguments being passed into synchronous open call.");
  }
  if (!args[0].isString() || !args[1].isNumber() || !args[2].isNumber()) {
    throw jsi::JSError(
        rt, "Incorrectly typed objects passed into synchronous open call.");
  }
  std::string filenameUTF8 = args[0].asString(rt).utf8(rt);

  llvh::SmallString<32> fullFileName{rs.getDirname()};
  canonicalizePath(fullFileName, filenameUTF8);

  int flags = args[1].asNumber();
  int mode = args[2].asNumber();
  uv_fs_t openReq;
  int fd = uv_fs_open(
      rs.getLoop(), &openReq, fullFileName.c_str(), flags, mode, nullptr);
  if (fd < 0)
    throw jsi::JSError(
        rt,
        "OpenSync failed on file '" + filenameUTF8 + "' with errno " +
            std::to_string(errno) + ": " + std::strerror(errno));
  return jsi::Value(fd);
}

/// Closes the file descriptor passed in.
/// Called by js the following way:
/// binding.close(fd, undefined, ctx);
static jsi::Value
close(RuntimeState &rs, const jsi::Value *args, size_t count) {
  jsi::Runtime &rt = rs.getRuntime();
  if (count < 3) {
    throw jsi::JSError(
        rt, "Not enough arguments being passed into synchronous close call.");
  }
  if (!args[0].isNumber()) {
    throw jsi::JSError(
        rt, "Incorrectly typed objects passed into synchronous close call.");
  }
  int fd = args[0].getNumber();
  uv_fs_t close_req;
  int closeRes = uv_fs_close(rs.getLoop(), &close_req, fd, nullptr);
  if (closeRes < 0)
    throw jsi::JSError(
        rt,
        "Close failed on fd " + std::to_string(fd) + " with errno " +
            std::to_string(errno) + ": " + std::strerror(errno));
  return jsi::Value::undefined();
}

/// Returns information about the already opened file descriptor.
/// Called by js the following way:
/// fd = binding.fstat(fd, use_bigint, undefined, ctx)
static jsi::Value
fstat(RuntimeState &rs, const jsi::Value *args, size_t count) {
  jsi::Runtime &rt = rs.getRuntime();
  if (count < 4) {
    throw jsi::JSError(
        rt, "Not enough arguments being passed into synchronous fstat call.");
  }
  uv_fs_t fstat_req;
  int fd = args[0].asNumber();
  int fstatRes = uv_fs_fstat(rs.getLoop(), &fstat_req, fd, nullptr);

  if (fstatRes < 0)
    throw jsi::JSError(
        rt,
        "Fstat failed on fd " + std::to_string(fd) + " with errno " +
            std::to_string(errno) + ": " + std::strerror(errno));

  uv_stat_t *statbuf = uv_fs_get_statbuf(&fstat_req);
  jsi::Object res{rt};

  // Missing properties: atime, mtime, ctime, birthtime because no datetime
  // support
  res.setProperty(rt, "dev", (double)statbuf->st_dev);
  res.setProperty(rt, "mode", (double)statbuf->st_mode);
  res.setProperty(rt, "nlink", (double)statbuf->st_nlink);
  res.setProperty(rt, "uid", (double)statbuf->st_uid);
  res.setProperty(rt, "gid", (double)statbuf->st_gid);
  res.setProperty(rt, "rdev", (double)statbuf->st_rdev);
  res.setProperty(rt, "blksize", (double)statbuf->st_blksize);
  res.setProperty(rt, "ino", (double)statbuf->st_size);
  res.setProperty(rt, "size", (double)statbuf->st_size);
  res.setProperty(rt, "blocks", (double)statbuf->st_blocks);

  return res;
}

/// Initializes a new JS function given a function pointer to the c++ function.
static void defineJSFunction(
    RuntimeState &rs,
    jsi::Value (*functionPtr)(RuntimeState &, const jsi::Value *, size_t),
    const std::string &functionName,
    size_t numArgs,
    jsi::Object &fs) {
  jsi::Runtime &rt = rs.getRuntime();
  jsi::String jsiFunctionName = jsi::String::createFromAscii(rt, functionName);
  jsi::Object JSFunction = jsi::Function::createFromHostFunction(
      rt,
      jsi::PropNameID::forString(rt, jsiFunctionName),
      numArgs,
      [&rs, functionPtr](
          jsi::Runtime &rt,
          const jsi::Value &,
          const jsi::Value *args,
          size_t count) -> jsi::Value { return functionPtr(rs, args, count); });
  fs.setProperty(rt, jsiFunctionName, JSFunction);
}

/// Adds the 'fs' object as a property of internalBinding.
jsi::Value facebook::fsBinding(RuntimeState &rs) {
  jsi::Runtime &rt = rs.getRuntime();
  jsi::Object fs{rt};

  defineJSFunction(rs, open, "open", 5, fs);
  defineJSFunction(rs, close, "close", 3, fs);
  defineJSFunction(rs, fstat, "fstat", 4, fs);

  jsi::String fsLabel = jsi::String::createFromAscii(rt, "fs");
  rs.setInternalBindingProp(fsLabel, std::move(fs));
  return rs.getInternalBindingProp(fsLabel);
}