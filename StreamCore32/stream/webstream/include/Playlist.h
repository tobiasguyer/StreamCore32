#pragma once
#include <string>
#include <optional>
#include <algorithm>
#include "StreamBase.h"

namespace streamcore::helpers {

inline bool hasPlaylistExt(const std::string& u){ auto L=StreamBase::toLower(u); return StreamBase::endsWith(L, ".m3u")||StreamBase::endsWith(L, ".m3u8")||StreamBase::endsWith(L, ".pls"); }

inline bool isPlaylistContentType(const std::string& ct){ auto L=StreamBase::toLower(ct); return StreamBase::startsWith(L,"audio/x-mpegurl")||StreamBase::startsWith(L,"application/vnd.apple.mpegurl")||StreamBase::startsWith(L,"application/x-mpegURL")||StreamBase::startsWith(L,"application/pls")||StreamBase::startsWith(L,"audio/x-scpls")||StreamBase::startsWith(L,"text/"); }

inline std::optional<std::string> parsePlaylistBody(const std::string& body){ size_t pos=0; bool first=true; while(pos<body.size()){ size_t end=body.find_first_of("\r\n", pos); size_t len=(end==std::string::npos)?(body.size()-pos):(end-pos); std::string line=body.substr(pos,len); if(end==std::string::npos) pos=body.size(); else pos=(body[end]=='\r' && end+1<body.size() && body[end+1]=='\n')? end+2 : end+1; if(first && line.size()>=3 && (unsigned char)line[0]==0xEF && (unsigned char)line[1]==0xBB && (unsigned char)line[2]==0xBF) line.erase(0,3); first=false; StreamBase::trim(line); if(line.empty()) continue; if(line[0]=='#'||line[0]==';'||line[0]=='[') continue; auto eq=line.find('='); std::string cand=(eq!=std::string::npos)?line.substr(eq+1):line; StreamBase::trim(cand); if(StreamBase::startsWith(cand,"http://")||StreamBase::startsWith(cand,"https://")) return cand; } return std::nullopt; }

} // namespace