/* Copyright (c) 2012-2015 LevelDOWN contributors
 * See list at <https://github.com/level/leveldown#contributing>
 * MIT License <https://github.com/level/leveldown/blob/master/LICENSE.md>
 */

#include <node.h>
#include <node_buffer.h>

#include <leveldb/db.h>
#include <leveldb/write_batch.h>

#include "leveldown.h"
#include "database.h"
#include "async.h"
#include "database_async.h"
#include "batch.h"
#include "iterator.h"

namespace leveldown {

static v8::Persistent<v8::FunctionTemplate> database_constructor;

Database::Database (NanUtf8String* location) : location(location) {
  db = NULL;
  currentIteratorId = 0;
  pendingCloseWorker = NULL;
  blockCache = NULL;
  filterPolicy = NULL;
};

Database::~Database () {
  if (db != NULL)
    delete db;
  delete location;
};

NanUtf8String* Database::Location() { return location; }

/* Calls from worker threads, NO V8 HERE *****************************/

leveldb::Status Database::OpenDatabase (
        leveldb::Options* options
      , std::string location
    ) {
  return leveldb::DB::Open(*options, location, &db);
}

leveldb::Status Database::PutToDatabase (
        leveldb::WriteOptions* options
      , leveldb::Slice key
      , leveldb::Slice value
    ) {
  return db->Put(*options, key, value);
}

leveldb::Status Database::GetFromDatabase (
        leveldb::ReadOptions* options
      , leveldb::Slice key
      , std::string& value
    ) {
  return db->Get(*options, key, &value);
}

leveldb::Status Database::DeleteFromDatabase (
        leveldb::WriteOptions* options
      , leveldb::Slice key
    ) {
  return db->Delete(*options, key);
}

leveldb::Status Database::WriteBatchToDatabase (
        leveldb::WriteOptions* options
      , leveldb::WriteBatch* batch
    ) {
  return db->Write(*options, batch);
}

uint64_t Database::ApproximateSizeFromDatabase (const leveldb::Range* range) {
  uint64_t size;
  db->GetApproximateSizes(range, 1, &size);
  return size;
}

void Database::GetPropertyFromDatabase (
      const leveldb::Slice& property
    , std::string* value) {

  db->GetProperty(property, value);
}

leveldb::Iterator* Database::NewIterator (leveldb::ReadOptions* options) {
  return db->NewIterator(*options);
}

const leveldb::Snapshot* Database::NewSnapshot () {
  return db->GetSnapshot();
}

void Database::ReleaseSnapshot (const leveldb::Snapshot* snapshot) {
  return db->ReleaseSnapshot(snapshot);
}

void Database::ReleaseIterator (uint32_t id) {
  // called each time an Iterator is End()ed, in the main thread
  // we have to remove our reference to it and if it's the last iterator
  // we have to invoke a pending CloseWorker if there is one
  // if there is a pending CloseWorker it means that we're waiting for
  // iterators to end before we can close them
  iterators.erase(id);
  if (iterators.empty() && pendingCloseWorker != NULL) {
    NanAsyncQueueWorker((AsyncWorker*)pendingCloseWorker);
    pendingCloseWorker = NULL;
  }
}

void Database::CloseDatabase () {
  delete db;
  db = NULL;
  if (blockCache) {
    delete blockCache;
    blockCache = NULL;
  }
  if (filterPolicy) {
    delete filterPolicy;
    filterPolicy = NULL;
  }
}

/* V8 exposed functions *****************************/

NAN_METHOD(LevelDOWN) {
  NanScope();

  v8::Local<v8::String> location;
  if (args.Length() != 0 && args[0]->IsString())
    location = args[0].As<v8::String>();
  NanReturnValue(Database::NewInstance(location));
}

void Database::Init () {
  v8::Local<v8::FunctionTemplate> tpl = NanNew<v8::FunctionTemplate>(Database::New);
  NanAssignPersistent(database_constructor, tpl);
  tpl->SetClassName(NanNew("Database"));
  tpl->InstanceTemplate()->SetInternalFieldCount(1);
  NODE_SET_PROTOTYPE_METHOD(tpl, "open", Database::Open);
  NODE_SET_PROTOTYPE_METHOD(tpl, "close", Database::Close);
  NODE_SET_PROTOTYPE_METHOD(tpl, "put", Database::Put);
  NODE_SET_PROTOTYPE_METHOD(tpl, "get", Database::Get);
  NODE_SET_PROTOTYPE_METHOD(tpl, "del", Database::Delete);
  NODE_SET_PROTOTYPE_METHOD(tpl, "batch", Database::Batch);
  NODE_SET_PROTOTYPE_METHOD(tpl, "approximateSize", Database::ApproximateSize);
  NODE_SET_PROTOTYPE_METHOD(tpl, "getProperty", Database::GetProperty);
  NODE_SET_PROTOTYPE_METHOD(tpl, "iterator", Database::Iterator);
}

NAN_METHOD(Database::New) {
  NanScope();

  NanUtf8String* location = new NanUtf8String(args[0]);

  Database* obj = new Database(location);
  obj->Wrap(args.This());

  NanReturnValue(args.This());
}

v8::Handle<v8::Value> Database::NewInstance (v8::Local<v8::String> &location) {
  NanEscapableScope();

  v8::Local<v8::Object> instance;

  v8::Local<v8::FunctionTemplate> constructorHandle =
      NanNew<v8::FunctionTemplate>(database_constructor);

  if (location.IsEmpty()) {
    instance = constructorHandle->GetFunction()->NewInstance(0, NULL);
  } else {
    v8::Handle<v8::Value> argv[] = { location };
    instance = constructorHandle->GetFunction()->NewInstance(1, argv);
  }

  return NanEscapeScope(instance);
}

NAN_METHOD(Database::Open) {
  NanScope();

  LD_METHOD_SETUP_COMMON(open, 0, 1)

  bool createIfMissing = NanBooleanOptionValue(
      optionsObj
    , NanNew("createIfMissing")
    , true
  );
  bool errorIfExists =
      NanBooleanOptionValue(optionsObj, NanNew("errorIfExists"));
  bool compression =
      NanBooleanOptionValue(optionsObj, NanNew("compression"), true);

  uint32_t cacheSize = NanUInt32OptionValue(
      optionsObj
    , NanNew("cacheSize")
    , 8 << 20
  );
  uint32_t writeBufferSize = NanUInt32OptionValue(
      optionsObj
    , NanNew("writeBufferSize")
    , 4 << 20
  );
  uint32_t blockSize = NanUInt32OptionValue(
      optionsObj
    , NanNew("blockSize")
    , 4096
  );
  uint32_t maxOpenFiles = NanUInt32OptionValue(
      optionsObj
    , NanNew("maxOpenFiles")
    , 1000
  );
  uint32_t blockRestartInterval = NanUInt32OptionValue(
      optionsObj
    , NanNew("blockRestartInterval")
    , 16
  );

  database->blockCache = leveldb::NewLRUCache(cacheSize);
  database->filterPolicy = leveldb::NewBloomFilterPolicy(10);

  OpenWorker* worker = new OpenWorker(
      database
    , new NanCallback(callback)
    , database->blockCache
    , database->filterPolicy
    , createIfMissing
    , errorIfExists
    , compression
    , writeBufferSize
    , blockSize
    , maxOpenFiles
    , blockRestartInterval
  );
  // persist to prevent accidental GC
  v8::Local<v8::Object> _this = args.This();
  worker->SaveToPersistent("database", _this);
  NanAsyncQueueWorker(worker);

  NanReturnUndefined();
}

NAN_METHOD(Database::Close) {
  NanScope();

  LD_METHOD_SETUP_COMMON_ONEARG(close)

  CloseWorker* worker = new CloseWorker(
      database
    , new NanCallback(callback)
  );
  // persist to prevent accidental GC
  v8::Local<v8::Object> _this = args.This();
  worker->SaveToPersistent("database", _this);

  if (!database->iterators.empty()) {
    // yikes, we still have iterators open! naughty naughty.
    // we have to queue up a CloseWorker and manually close each of them.
    // the CloseWorker will be invoked once they are all cleaned up
    database->pendingCloseWorker = worker;

    for (
        std::map< uint32_t, leveldown::Iterator * >::iterator it
            = database->iterators.begin()
      ; it != database->iterators.end()
      ; ++it) {

        // for each iterator still open, first check if it's already in
        // the process of ending (ended==true means an async End() is
        // in progress), if not, then we call End() with an empty callback
        // function and wait for it to hit ReleaseIterator() where our
        // CloseWorker will be invoked

        /*
        v8::Local<v8::Object> localHandle = NanNew(it->second);
        leveldown::Iterator* iterator =
            node::ObjectWrap::Unwrap<leveldown::Iterator>(localHandle->
                Get(NanNew("iterator")).As<v8::Object>());
                */
        leveldown::Iterator *iterator = it->second;

        if (!iterator->ended) {
          v8::Local<v8::Function> end =
              v8::Local<v8::Function>::Cast(NanObjectWrapHandle(iterator)->Get(
                  NanNew<v8::String>("end")));
          v8::Local<v8::Value> argv[] = {
              NanNew<v8::FunctionTemplate>()->GetFunction() // empty callback
          };
          NanMakeCallback(
              NanObjectWrapHandle(iterator)
            , end
            , 1
            , argv
          );
        }
    }
  } else {
    NanAsyncQueueWorker(worker);
  }

  NanReturnUndefined();
}

NAN_METHOD(Database::Put) {
  NanScope();

  LD_METHOD_SETUP_COMMON(put, 2, 3)

  v8::Local<v8::Object> keyHandle = args[0].As<v8::Object>();
  v8::Local<v8::Object> valueHandle = args[1].As<v8::Object>();
  LD_STRING_OR_BUFFER_TO_SLICE(key, keyHandle, key)
  LD_STRING_OR_BUFFER_TO_SLICE(value, valueHandle, value)

  bool sync = NanBooleanOptionValue(optionsObj, NanNew("sync"));

  WriteWorker* worker  = new WriteWorker(
      database
    , new NanCallback(callback)
    , key
    , value
    , sync
    , keyHandle
    , valueHandle
  );

  // persist to prevent accidental GC
  v8::Local<v8::Object> _this = args.This();
  worker->SaveToPersistent("database", _this);
  NanAsyncQueueWorker(worker);

  NanReturnUndefined();
}

NAN_METHOD(Database::Get) {
  NanScope();

  LD_METHOD_SETUP_COMMON(get, 1, 2)

  v8::Local<v8::Object> keyHandle = args[0].As<v8::Object>();
  LD_STRING_OR_BUFFER_TO_SLICE(key, keyHandle, key)

  bool asBuffer = NanBooleanOptionValue(optionsObj, NanNew("asBuffer"), true);
  bool fillCache = NanBooleanOptionValue(optionsObj, NanNew("fillCache"), true);

  ReadWorker* worker = new ReadWorker(
      database
    , new NanCallback(callback)
    , key
    , asBuffer
    , fillCache
    , keyHandle
  );
  // persist to prevent accidental GC
  v8::Local<v8::Object> _this = args.This();
  worker->SaveToPersistent("database", _this);
  NanAsyncQueueWorker(worker);

  NanReturnUndefined();
}

NAN_METHOD(Database::Delete) {
  NanScope();

  LD_METHOD_SETUP_COMMON(del, 1, 2)

  v8::Local<v8::Object> keyHandle = args[0].As<v8::Object>();
  LD_STRING_OR_BUFFER_TO_SLICE(key, keyHandle, key)

  bool sync = NanBooleanOptionValue(optionsObj, NanNew("sync"));

  DeleteWorker* worker = new DeleteWorker(
      database
    , new NanCallback(callback)
    , key
    , sync
    , keyHandle
  );
  // persist to prevent accidental GC
  v8::Local<v8::Object> _this = args.This();
  worker->SaveToPersistent("database", _this);
  NanAsyncQueueWorker(worker);

  NanReturnUndefined();
}

NAN_METHOD(Database::Batch) {
  NanScope();

  if ((args.Length() == 0 || args.Length() == 1) && !args[0]->IsArray()) {
    v8::Local<v8::Object> optionsObj;
    if (args.Length() > 0 && args[0]->IsObject()) {
      optionsObj = args[0].As<v8::Object>();
    }
    NanReturnValue(Batch::NewInstance(args.This(), optionsObj));
  }

  LD_METHOD_SETUP_COMMON(batch, 1, 2)

  bool sync = NanBooleanOptionValue(optionsObj, NanNew("sync"));

  v8::Local<v8::Array> array = v8::Local<v8::Array>::Cast(args[0]);

  leveldb::WriteBatch* batch = new leveldb::WriteBatch();
  bool hasData = false;

  for (unsigned int i = 0; i < array->Length(); i++) {
    if (!array->Get(i)->IsObject())
      continue;

    v8::Local<v8::Object> obj = v8::Local<v8::Object>::Cast(array->Get(i));
    v8::Local<v8::Value> keyBuffer = obj->Get(NanNew("key"));
    v8::Local<v8::Value> type = obj->Get(NanNew("type"));

    if (type->StrictEquals(NanNew("del"))) {
      LD_STRING_OR_BUFFER_TO_SLICE(key, keyBuffer, key)

      batch->Delete(key);
      if (!hasData)
        hasData = true;

      DisposeStringOrBufferFromSlice(keyBuffer, key);
    } else if (type->StrictEquals(NanNew("put"))) {
      v8::Local<v8::Value> valueBuffer = obj->Get(NanNew("value"));

      LD_STRING_OR_BUFFER_TO_SLICE(key, keyBuffer, key)
      LD_STRING_OR_BUFFER_TO_SLICE(value, valueBuffer, value)
      batch->Put(key, value);
      if (!hasData)
        hasData = true;

      DisposeStringOrBufferFromSlice(keyBuffer, key);
      DisposeStringOrBufferFromSlice(valueBuffer, value);
    }
  }

  // don't allow an empty batch through
  if (hasData) {
    BatchWorker* worker = new BatchWorker(
        database
      , new NanCallback(callback)
      , batch
      , sync
    );
    // persist to prevent accidental GC
    v8::Local<v8::Object> _this = args.This();
    worker->SaveToPersistent("database", _this);
    NanAsyncQueueWorker(worker);
  } else {
    LD_RUN_CALLBACK(callback, 0, NULL);
  }

  NanReturnUndefined();
}

NAN_METHOD(Database::ApproximateSize) {
  NanScope();

  v8::Local<v8::Object> startHandle = args[0].As<v8::Object>();
  v8::Local<v8::Object> endHandle = args[1].As<v8::Object>();

  LD_METHOD_SETUP_COMMON(approximateSize, -1, 2)

  LD_STRING_OR_BUFFER_TO_SLICE(start, startHandle, start)
  LD_STRING_OR_BUFFER_TO_SLICE(end, endHandle, end)

  ApproximateSizeWorker* worker  = new ApproximateSizeWorker(
      database
    , new NanCallback(callback)
    , start
    , end
    , startHandle
    , endHandle
  );
  // persist to prevent accidental GC
  v8::Local<v8::Object> _this = args.This();
  worker->SaveToPersistent("database", _this);
  NanAsyncQueueWorker(worker);

  NanReturnUndefined();
}

NAN_METHOD(Database::GetProperty) {
  NanScope();

  v8::Local<v8::Value> propertyHandle = args[0].As<v8::Object>();
  v8::Local<v8::Function> callback; // for LD_STRING_OR_BUFFER_TO_SLICE

  LD_STRING_OR_BUFFER_TO_SLICE(property, propertyHandle, property)

  leveldown::Database* database =
      node::ObjectWrap::Unwrap<leveldown::Database>(args.This());

  std::string* value = new std::string();
  database->GetPropertyFromDatabase(property, value);
  v8::Local<v8::String> returnValue
      = NanNew<v8::String>(value->c_str(), value->length());
  delete value;
  delete[] property.data();

  NanReturnValue(returnValue);
}

NAN_METHOD(Database::Iterator) {
  NanScope();

  Database* database = node::ObjectWrap::Unwrap<Database>(args.This());

  v8::Local<v8::Object> optionsObj;
  if (args.Length() > 0 && args[0]->IsObject()) {
    optionsObj = v8::Local<v8::Object>::Cast(args[0]);
  }

  // each iterator gets a unique id for this Database, so we can
  // easily store & lookup on our `iterators` map
  uint32_t id = database->currentIteratorId++;
  v8::TryCatch try_catch;
  v8::Local<v8::Object> iteratorHandle = Iterator::NewInstance(
      args.This()
    , NanNew<v8::Number>(id)
    , optionsObj
  );
  if (try_catch.HasCaught()) {
    // NB: node::FatalException can segfault here if there is no room on stack.
    return NanThrowError("Fatal Error in Database::Iterator!");
  }

  leveldown::Iterator *iterator =
      node::ObjectWrap::Unwrap<leveldown::Iterator>(iteratorHandle);

  database->iterators[id] = iterator;

  // register our iterator
  /*
  v8::Local<v8::Object> obj = NanNew<v8::Object>();
  obj->Set(NanNew("iterator"), iteratorHandle);
  v8::Persistent<v8::Object> persistent;
  persistent.Reset(nan_isolate, obj);
  database->iterators.insert(std::pair< uint32_t, v8::Persistent<v8::Object> & >
      (id, persistent));
  */

  NanReturnValue(iteratorHandle);
}


} // namespace leveldown
