/*
 * SessionRmdNotebook.cpp
 *
 * Copyright (C) 2009-16 by RStudio, Inc.
 *
 * Unless you have received this program directly from RStudio pursuant
 * to the terms of a commercial license agreement with RStudio, then
 * this program is licensed to you under the terms of version 3 of the
 * GNU Affero General Public License. This program is distributed WITHOUT
 * ANY EXPRESS OR IMPLIED WARRANTY, INCLUDING THOSE OF NON-INFRINGEMENT,
 * MERCHANTABILITY OR FITNESS FOR A PARTICULAR PURPOSE. Please refer to the
 * AGPL (http://www.gnu.org/licenses/agpl-3.0.txt) for more details.
 *
 */

#include "SessionRmdNotebook.hpp"

#include <iostream>

#include <boost/foreach.hpp>

#include <r/RJson.hpp>
#include <r/RExec.hpp>

#include <core/Exec.hpp>
#include <core/FileSerializer.hpp>
#include <core/Algorithm.hpp>
#include <core/json/Json.hpp>
#include <core/json/JsonRpc.hpp>

#include <session/SessionModuleContext.hpp>
#include <session/SessionSourceDatabase.hpp>

#define kChunkDefs        "chunk_definitions"
#define kChunkDocId       "doc_id"
#define kChunkId          "chunk_id"
#define kChunkLibDir      "lib"
#define kChunkOutputPath  "chunk_output"
#define kChunkUrl         "url"

using namespace rstudio::core;

namespace rstudio {
namespace session {
namespace modules {
namespace rmarkdown {
namespace notebook {

namespace {

// A notebook .Rmd is accompanied by a sidecar .Rnb.cached folder, which has
// the following structure:
//
// - foo.Rmd
// + foo.Rnd.cached
//   - chunks.json
//   - cwiaiw9i4f0.html
//   + cwiaiw9i4f0_files
//     - plot.png
//   - c0aj9vhk0cz.html
//   + lib
//     + htmlwidgets
//       - htmlwidget.js
// 
// That is:
// - each chunk has an ID and is represented by a single, self-contained HTML
//   file, with a separate folder for dependencies
// - dependencies of each chunk are in a folder alongside the chunk
// - the special file "chunks.json" indicates the location of the chunks
//   in the source .Rmd
// - the special folder "lib" is used for shared libraries (e.g. scripts upon
//   which several htmlwidget chunks depend)


FilePath chunkCacheFolder(const std::string& docPath, const std::string& docId)
{
   FilePath folder;
   std::string stem;
   if (docPath.empty()) 
   {
      // the doc hasn't been saved, so keep its chunk output in the scratch
      // path
      folder = module_context::userScratchPath().childPath("unsaved-notebooks");
      stem = docId;
   }
   else
   {
      // the doc has been saved, so keep its chunk output alongside the doc
      // itself
      FilePath path = module_context::resolveAliasedPath(docPath);
      stem = path.stem();
      folder = path.parent();
   }

   return folder.childPath(stem + ".Rnb.cached");
}


FilePath chunkDefinitionsPath(
      const std::string& docPath, const std::string& docId)
{
   return chunkCacheFolder(docPath, docId).childPath("chunks.json");
}

FilePath chunkOutputPath(
      const std::string& docPath, const std::string& docId,
      const std::string& chunkId)
{
   return chunkCacheFolder(docPath, docId)
                          .childPath(chunkId + ".html");
}


Error enqueueChunkOutput(
      const std::string& docPath, const std::string& docId,
      const std::string& chunkId)
{
   json::Object output;
   output[kChunkUrl] = kChunkOutputPath "/" + docId + "/" + chunkId + ".html";
   output[kChunkId] = chunkId;
   output[kChunkDocId] = docId;
   ClientEvent event(client_events::kChunkOutput, output);
   module_context::enqueClientEvent(event);

   return Success();
}

Error executeInlineChunk(const json::JsonRpcRequest& request,
                         json::JsonRpcResponse*)
{
   std::string docPath, docId, chunkId, options, content;
   Error error = json::readParams(request.params, &docPath, &docId, &chunkId, 
         &options, &content);
   if (error)
      return error;

   // ensure we have a place to put the output
   FilePath chunkOutput = chunkOutputPath(docPath, docId, chunkId);
   error = chunkOutput.parent().ensureDirectory();
   if (error)
      return error;

   // ensure we have a library path
   FilePath chunkLibDir = chunkCacheFolder(docPath, docId).complete(
         kChunkLibDir);
   error = chunkLibDir.ensureDirectory();
   if (error)
      return error;

   // render the contents to the cached folder
   error = r::exec::RFunction(".rs.executeSingleChunk", options, content,
         chunkLibDir.absolutePath(),
         chunkOutput.absolutePath()).call();
   if (error)
      return error;

   error = enqueueChunkOutput(docPath, docId, chunkId);

   return Success();
}

void extractChunkIds(const json::Array& chunkOutputs, 
                     std::vector<std::string> *pIds)
{
   BOOST_FOREACH(const json::Value& chunkOutput, chunkOutputs)
   {
      if (chunkOutput.type() != json::ObjectType)
         continue;
      std::string chunkId;
      if (json::readObject(chunkOutput.get_obj(), kChunkId, &chunkId) ==
            Success()) 
      {
         pIds->push_back(chunkId);
      }
   }
}

void replayChunkOutputs(const std::string& docPath, const std::string& docId,
      const std::string& requestId, const json::Array& chunkOutputs) 
{
   std::vector<std::string> chunkIds;
   extractChunkIds(chunkOutputs, &chunkIds);

   // find all the chunks and play them back to the client
   BOOST_FOREACH(const std::string& chunkId, chunkIds)
   {
      enqueueChunkOutput(docPath, docId, chunkId);
   }

   json::Object result;
   result["path"] = docPath;
   result["request_id"] = requestId;
   ClientEvent event(client_events::kChunkOutputFinished, result);
   module_context::enqueClientEvent(event);
}

// called by the client to inject output into a recently opened document 
Error refreshChunkOutput(const json::JsonRpcRequest& request,
                     json::JsonRpcResponse*)
{
   // extract path to doc to be refreshed
   std::string docPath, docId, requestId;
   Error error = json::readParams(request.params, &docPath, &docId, &requestId);
   if (error)
      return error;

   json::Object result;
   json::Value chunkDefs; 
   error = getChunkDefs(docPath, docId, &chunkDefs);

   // schedule the work to play back the chunks (we don't do it synchronously
   // so the RPC can return immediately)
   if (!error && chunkDefs.type() == json::ArrayType) 
   {
      module_context::scheduleDelayedWork(boost::posix_time::milliseconds(10), 
            boost::bind(replayChunkOutputs, docPath, docId, requestId, 
                        chunkDefs.get_array()));
   }

   return Success();
}

bool copyCacheItem(const FilePath& from,
                   const FilePath& to,
                   const FilePath& path)
{

   std::string relativePath = path.relativePath(from);
   FilePath target = to.complete(relativePath);

   Error error = path.isDirectory() ?
                     target.ensureDirectory() :
                     path.copy(target);
   if (error)
      LOG_ERROR(error);

   return true;
}

Error copyCache(const FilePath& from, const FilePath& to)
{
   Error error = to.ensureDirectory();
   if (error)
      return error;

   return from.childrenRecursive(
             boost::bind(copyCacheItem, from, to, _2));
}

void onDocRemoved(const std::string& docId)
{
   // check to see if this document was an unsaved notebook, and clean up its
   // cache folder if so
   FilePath cacheFolder = chunkCacheFolder("", docId);
   Error error = cacheFolder.removeIfExists();
   if (error)
      LOG_ERROR(error);
}

void onDocRenamed(const std::string& oldPath, 
                  boost::shared_ptr<source_database::SourceDocument> pDoc)
{
   // compute cache folders and ignore if we can't safely adjust them
   FilePath oldCacheDir = chunkCacheFolder(oldPath, pDoc->id());
   FilePath newCacheDir = chunkCacheFolder(pDoc->path(), pDoc->id());
   if (!oldCacheDir.exists() || newCacheDir.exists())
      return;

   // if the doc was previously unsaved, we can just move the whole folder 
   // to its newly saved location
   if (oldPath.empty())
   {
      oldCacheDir.move(newCacheDir);
      return;
   }

   Error error = copyCache(oldCacheDir, newCacheDir);
   if (error)
      LOG_ERROR(error);
}

Error handleChunkOutputRequest(const http::Request& request,
                               http::Response* pResponse)
{
   // uri format is: /chunk_output/<doc-id>/...
   
   // split URI into pieces, extract the document ID, and remove that part of
   // the URI
   std::vector<std::string> parts = algorithm::split(request.uri(), "/");
   if (parts.size() < 4) 
      return Success();
   std::string docId = parts[2];
   for (int i = 0; i < 3; i++)
      parts.erase(parts.begin());

   std::string path;
   Error error = source_database::getPath(docId, &path);
   if (error)
      return error;
   FilePath target = chunkCacheFolder(path, docId).complete(
         algorithm::join(parts, "/"));

   if (parts[0] == kChunkLibDir)
   {
      // if a reference to the chunk library folder, we can reuse the contents
      pResponse->setCacheableFile(target, request);
   }
   else
   {
      // otherwise, we don't want the output to be cached (so we get fresh
      // chunk output every time)
      pResponse->setNoCacheHeaders();
      pResponse->setFile(target, request);
   }

   return Success();
}

// given and old and new set of chunk definitions, cleans up all the chunks
// files in the old set but not in the new set
void cleanChunks(const FilePath& cacheDir,
                 const json::Array &oldDefs, 
                 const json::Array &newDefs)
{
   Error error;
   std::vector<std::string> oldIds, newIds;

   // extract chunk IDs from JSON objects
   extractChunkIds(oldDefs, &oldIds);
   extractChunkIds(newDefs, &newIds);

   // compute the set of stale IDs
   std::vector<std::string> staleIds;
   std::sort(oldIds.begin(), oldIds.end());
   std::sort(newIds.begin(), newIds.end());
   std::set_difference(oldIds.begin(), oldIds.end(),
                       newIds.begin(), newIds.end(), 
                       std::back_inserter(staleIds));

   BOOST_FOREACH(const std::string& staleId, staleIds)
   {
      // clean chunk HTML and supporting files if present
      error = cacheDir.complete(staleId + ".html").removeIfExists();
      if (error)
         LOG_ERROR(error);
      cacheDir.complete(staleId + "_files").removeIfExists();
      if (error)
         LOG_ERROR(error);
   }
}

} // anonymous namespace

Error setChunkDefs(const std::string& docPath, const std::string& docId,
                   const json::Array& newDefs)
{
   // create JSON object wrapping 
   json::Object chunkDefs;
   chunkDefs[kChunkDefs] = newDefs;

   // ensure we have a place to write the sidecar file
   FilePath defFile = chunkDefinitionsPath(docPath, docId);
   Error error = defFile.parent().ensureDirectory();
   if (error)
      return error;

   // get the old set of chunk IDs so we can clean up any not in the new set 
   // of chunks
   std::vector<std::string> chunkIds;
   json::Value oldDefs;
   error = getChunkDefs(docPath, docId, &oldDefs);
   if (error)
      LOG_ERROR(error);
   else if (oldDefs.type() == json::ArrayType)
      cleanChunks(chunkCacheFolder(docPath, docId),
                  oldDefs.get_array(), newDefs);

   // write to the sidecar file
   std::ostringstream oss;
   json::write(chunkDefs, oss);
   return writeStringToFile(defFile, oss.str());
}

Error getChunkDefs(const std::string& docPath, const std::string& docId,
                   core::json::Value* pDefs)
{
   Error error;
   FilePath defs = chunkDefinitionsPath(docPath, docId);
   if (!defs.exists())
      return Success();

   // read the defs file 
   std::string contents;
   error = readStringFromFile(defs, &contents);
   if (error)
      return error;

   // pull out the contents
   json::Value defContents;
   if (!json::parse(contents, &defContents) || 
       defContents.type() != json::ObjectType)
      return Error(json::errc::ParseError, ERROR_LOCATION);
   
   // extract the chunk definitions
   json::Array chunkDefs;
   error = json::readObject(defContents.get_obj(), kChunkDefs, &chunkDefs);
   if (error)
      return error;

   // return to caller
   *pDefs = chunkDefs;
   return Success();
}

Error initialize()
{
   using boost::bind;
   using namespace module_context;

   source_database::events().onDocRenamed.connect(onDocRenamed);
   source_database::events().onDocRemoved.connect(onDocRemoved);
   
   ExecBlock initBlock;
   initBlock.addFunctions()
      (bind(registerRpcMethod, "execute_inline_chunk", executeInlineChunk))
      (bind(registerRpcMethod, "refresh_chunk_output", refreshChunkOutput))
      (bind(registerUriHandler, "/" kChunkOutputPath, 
            handleChunkOutputRequest))
      (bind(module_context::sourceModuleRFile, "SessionRmdNotebook.R"));

   return initBlock.execute();
}

} // namespace notebook
} // namespace rmarkdown
} // namespace modules
} // namespace session
} // namespace rstudio

