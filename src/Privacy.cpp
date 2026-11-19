// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// rand_s (the system's random generator) needs this before <stdlib.h>.
#define _CRT_RAND_S
#include <stdlib.h>

#include "Privacy.h"

#include "Engine.h"
#include "Json.h"
#include "Util.h"

#include <cstdlib>
#include <cwctype>
#include <map>
#include <set>
#include <vector>

namespace privacy {

namespace {

struct SiteOverrides {
    int https { -1 };          // -1 = default, 0 = off, 1 = on
    int params { -1 };
    int fingerprinting { -1 };
};

std::map<std::wstring, SiteOverrides> g_sites;
std::set<std::wstring> g_httpThisSession;
std::string g_sessionKey;

std::wstring sitesFile() { return util::dataDirectory() + L"\\site-privacy.json"; }

std::wstring lower(std::wstring text)
{
    for (auto& c : text)
        c = static_cast<wchar_t>(towlower(c));
    return text;
}

std::wstring hostOf(const std::wstring& url)
{
    size_t start = url.find(L"://");
    if (start == std::wstring::npos)
        return { };
    start += 3;
    size_t end = url.find_first_of(L"/?#", start);
    std::wstring host = url.substr(start, end == std::wstring::npos ? std::wstring::npos : end - start);
    if (size_t at = host.rfind(L'@'); at != std::wstring::npos)
        host = host.substr(at + 1);
    if (!host.empty() && host[0] == L'[')
        return lower(host.substr(0, host.find(L']') + 1));
    return lower(host.substr(0, host.find(L':')));
}

void save()
{
    std::string json = "{";
    bool first = true;
    for (auto& [site, overrides] : g_sites) {
        if (overrides.https < 0 && overrides.params < 0 && overrides.fingerprinting < 0)
            continue;
        json += (first ? "" : ",") + util::jsonString(site) + ":{";
        std::string fields;
        auto field = [&](const char* name, int value) {
            if (value >= 0)
                fields += std::string(fields.empty() ? "" : ",") + "\"" + name + "\":" + (value ? "true" : "false");
        };
        field("https", overrides.https);
        field("params", overrides.params);
        field("fingerprinting", overrides.fingerprinting);
        json += fields + "}";
        first = false;
    }
    util::writeFileAtomically(sitesFile(), json + "}");
}

bool globalOn(const wchar_t* key)
{
    return util::readSetting(key, L"1") != L"0";
}

// Looks up a site's override, also for its parent domains (an exception
// for example.com covers www.example.com and shop.example.com).
int overrideFor(const std::wstring& url, int SiteOverrides::*field)
{
    std::wstring site = siteOf(url);
    while (!site.empty()) {
        auto found = g_sites.find(site);
        if (found != g_sites.end() && found->second.*field >= 0)
            return found->second.*field;
        size_t dot = site.find(L'.');
        if (dot == std::wstring::npos)
            break;
        site = site.substr(dot + 1);
    }
    return -1;
}

bool isLocalHost(const std::wstring& host)
{
    if (host.empty() || host == L"localhost" || host[0] == L'[')
        return true;
    if (host.size() > 10 && host.compare(host.size() - 10, 10, L".localhost") == 0)
        return true;
    for (const wchar_t* suffix : { L".local", L".lan", L".home", L".internal", L".test", L".arpa" }) {
        size_t length = wcslen(suffix);
        if (host.size() > length && host.compare(host.size() - length, length, suffix) == 0)
            return true;
    }
    // IP addresses (192.168.1.1) and single-label names (router) are local
    // or don't have certificates.
    bool digitsAndDots = true;
    for (wchar_t c : host)
        digitsAndDots = digitsAndDots && (iswdigit(c) || c == L'.');
    return digitsAndDots || host.find(L'.') == std::wstring::npos;
}

// Query parameters that only exist to track clicks (from Brave's and
// Firefox's lists). utm_* is matched by prefix.
bool isTrackingParameter(const std::wstring& name)
{
    static const std::set<std::wstring> names = {
        L"fbclid", L"gclid", L"gclsrc", L"dclid", L"gbraid", L"wbraid", L"msclkid", L"mc_eid", L"mc_cid",
        L"igshid", L"igsh", L"yclid", L"ysclid", L"twclid", L"ttclid", L"li_fat_id", L"epik", L"_hsenc", L"_hsmi",
        L"__hssc", L"__hstc", L"__hsfp", L"hsctatracking", L"mkt_tok", L"oly_anon_id", L"oly_enc_id", L"_openstat",
        L"rb_clickid", L"s_cid", L"vero_conv", L"vero_id", L"wickedid", L"ml_subscriber", L"ml_subscriber_hash",
        L"__s", L"_kx", L"irclickid", L"srsltid", L"si",
    };
    std::wstring key = lower(name);
    if (key.rfind(L"utm_", 0) == 0)
        return true;
    // "si" is Spotify's and YouTube's share tracker; elsewhere it can be real.
    return names.count(key) > 0;
}

std::string fingerprintScript()
{
    std::string off = "[";
    for (auto& [site, overrides] : g_sites) {
        if (overrides.fingerprinting == 0)
            off += (off.size() > 1 ? "," : "") + util::jsonString(site);
    }
    off += "]";
    // Runs first in every page and frame (page world). Values stay the same
    // within a site for this session, so pages see a consistent device, but
    // differ between sites and sessions, so they can't be used to follow you.
    return R"JS((function (config) {
  'use strict';
  if (location.protocol !== 'http:' && location.protocol !== 'https:') return;
  var host = location.hostname.toLowerCase().replace(/^www\./, '');
  if (/\.localhost$/.test(host)) return;
  for (var i = 0; i < config.off.length; i++) {
    var site = config.off[i];
    if (host === site || host.slice(-site.length - 1) === '.' + site) return;
  }
  var parts = host.split('.');
  var base = parts.length > 2 ? parts.slice(-2).join('.') : host;
  function hash(text) {
    var h = 2166136261;
    for (var i = 0; i < text.length; i++) { h ^= text.charCodeAt(i); h = Math.imul(h, 16777619); }
    return h >>> 0;
  }
  var seed = hash(config.key + '|' + base);
  function mix(n) {
    n = Math.imul(n ^ (n >>> 16), 0x45d9f3b);
    n = Math.imul(n ^ (n >>> 16), 0x45d9f3b);
    return (n ^ (n >>> 16)) >>> 0;
  }

  // Patched functions still look built in.
  var nativeToString = Function.prototype.toString;
  var originals = new WeakMap();
  function disguise(replacement, original) {
    originals.set(replacement, original);
    return replacement;
  }
  var toStringReplacement = disguise(function toString() {
    var original = originals.get(this);
    return nativeToString.call(original || this);
  }, nativeToString);
  Object.defineProperty(Function.prototype, 'toString', { value: toStringReplacement, writable: true, configurable: true });

  function patch(target, name, make) {
    if (!target) return;
    var descriptor = Object.getOwnPropertyDescriptor(target, name);
    if (!descriptor || typeof descriptor.value !== 'function') return;
    var original = descriptor.value;
    descriptor.value = disguise(make(original), original);
    Object.defineProperty(target, name, descriptor);
  }
  function patchGetter(target, name, value) {
    if (!target) return;
    var descriptor = Object.getOwnPropertyDescriptor(target, name);
    if (!descriptor || !descriptor.get) return;
    var original = descriptor.get;
    descriptor.get = disguise({ get: function () { return value(original.call(this)); } }.get, original);
    Object.defineProperty(target, name, descriptor);
  }

  // Pixels: the lowest bit of about 1 in 100 color values changes, always
  // the same ones for this site.
  function farble(data, width) {
    for (var i = 0; i < data.length; i += 4) {
      var m = mix(seed ^ (i >>> 2));
      if ((m & 127) === 0 && data[i + 3] !== 0) data[i + (m >>> 8) % 3] ^= 1;
    }
    return data;
  }

  var getImageData = CanvasRenderingContext2D.prototype.getImageData;
  patch(CanvasRenderingContext2D.prototype, 'getImageData', function (original) {
    return function getImageData() {
      var image = original.apply(this, arguments);
      try { farble(image.data, image.width); } catch (e) { /* leave it */ }
      return image;
    };
  });

  // toDataURL / toBlob: export a copy with the same change.
  function farbledCopy(canvas) {
    var width = canvas.width, height = canvas.height;
    if (!width || !height || width * height > 4000000) return null;
    var copy = document.createElement('canvas');
    copy.width = width;
    copy.height = height;
    var context = copy.getContext('2d');
    context.drawImage(canvas, 0, 0);
    var image = getImageData.call(context, 0, 0, width, height);
    farble(image.data, width);
    context.putImageData(image, 0, 0);
    return copy;
  }
  patch(HTMLCanvasElement.prototype, 'toDataURL', function (original) {
    return function toDataURL() {
      var copy = null;
      try { copy = farbledCopy(this); } catch (e) { copy = null; }
      return original.apply(copy || this, arguments);
    };
  });
  patch(HTMLCanvasElement.prototype, 'toBlob', function (original) {
    return function toBlob() {
      var copy = null;
      try { copy = farbledCopy(this); } catch (e) { copy = null; }
      return original.apply(copy || this, arguments);
    };
  });

  // WebGL: the graphics card's name reads like a Mac's (Prism says it's
  // Safari), and read-back pixels get the same small change.
  [window.WebGLRenderingContext, window.WebGL2RenderingContext].forEach(function (type) {
    if (!type) return;
    patch(type.prototype, 'getParameter', function (original) {
      return function getParameter(name) {
        if (name === 0x9245) return 'Apple Inc.';   // UNMASKED_VENDOR_WEBGL
        if (name === 0x9246) return 'Apple GPU';    // UNMASKED_RENDERER_WEBGL
        return original.apply(this, arguments);
      };
    });
    patch(type.prototype, 'readPixels', function (original) {
      return function readPixels() {
        var result = original.apply(this, arguments);
        var pixels = arguments[6];
        try { if (pixels && pixels.length) farble(pixels, arguments[2]); } catch (e) { /* leave it */ }
        return result;
      };
    });
  });

  // Audio: tiny changes (far below hearing) to analysed samples.
  function farbleAudio(array) {
    for (var i = 0; i < array.length; i += 1) {
      var m = mix(seed ^ i);
      if ((m & 15) === 0) array[i] = array[i] + ((m >>> 8) % 2 ? 1e-7 : -1e-7);
    }
  }
  var touched = new WeakSet();
  if (window.AudioBuffer) {
    patch(AudioBuffer.prototype, 'getChannelData', function (original) {
      return function getChannelData() {
        var data = original.apply(this, arguments);
        try {
          if (!touched.has(data)) { touched.add(data); farbleAudio(data); }
        } catch (e) { /* leave it */ }
        return data;
      };
    });
  }
  if (window.AnalyserNode) {
    patch(AnalyserNode.prototype, 'getFloatFrequencyData', function (original) {
      return function getFloatFrequencyData(array) {
        var result = original.apply(this, arguments);
        try { farbleAudio(array); } catch (e) { /* leave it */ }
        return result;
      };
    });
  }

  // Processor count: a common value, different per site.
  var Navigator = window.Navigator;
  if (Navigator) {
    patchGetter(Navigator.prototype, 'hardwareConcurrency', function (real) {
      var choices = [4, 8];
      var value = choices[seed % choices.length];
      return real && real < value ? real : value;
    });
  }
}))JS" "(" + std::string("{\"key\":") + json::quote(g_sessionKey) + ",\"off\":" + off + "});";
}

void applyFingerprintScript()
{
    std::vector<std::string> scripts;
    if (globalOn(L"Fingerprinting"))
        scripts.push_back(fingerprintScript());
    engine::setUserScripts("fingerprinting", std::move(scripts));
}

} // namespace

void initialize()
{
    // A new key every time Prism starts: fingerprints change between sessions.
    char key[33] = { };
    for (int i = 0; i < 4; ++i) {
        unsigned int value = 0;
        rand_s(&value);
        snprintf(key + i * 8, 9, "%08x", value);
    }
    g_sessionKey = key;

    json::Value root;
    if (json::parse(util::readFile(sitesFile()), root)) {
        for (auto& [site, value] : root.members()) {
            SiteOverrides overrides;
            if (value["https"].isBool()) overrides.https = value["https"].asBool() ? 1 : 0;
            if (value["params"].isBool()) overrides.params = value["params"].asBool() ? 1 : 0;
            if (value["fingerprinting"].isBool()) overrides.fingerprinting = value["fingerprinting"].asBool() ? 1 : 0;
            g_sites[util::fromUTF8(site)] = overrides;
        }
    }
    applyFingerprintScript();
}

std::wstring httpsMode()
{
    std::wstring mode = util::readSetting(L"HttpsMode", L"first");
    return mode == L"off" || mode == L"only" ? mode : L"first";
}

std::wstring siteOf(const std::wstring& url)
{
    std::wstring host = hostOf(url);
    if (host.rfind(L"www.", 0) == 0)
        host = host.substr(4);
    return host;
}

bool httpsFor(const std::wstring& url)
{
    int value = overrideFor(url, &SiteOverrides::https);
    return value >= 0 ? value == 1 : httpsMode() != L"off";
}

bool trackingParamsFor(const std::wstring& url)
{
    int value = overrideFor(url, &SiteOverrides::params);
    return value >= 0 ? value == 1 : globalOn(L"StripTrackingParams");
}

bool fingerprintingFor(const std::wstring& url)
{
    int value = overrideFor(url, &SiteOverrides::fingerprinting);
    return value >= 0 ? value == 1 : globalOn(L"Fingerprinting");
}

std::wstring removeTrackingParameters(const std::wstring& url)
{
    if (!trackingParamsFor(url))
        return url;
    size_t question = url.find(L'?');
    if (question == std::wstring::npos)
        return url;
    size_t hash = url.find(L'#', question);
    std::wstring query = url.substr(question + 1, hash == std::wstring::npos ? std::wstring::npos : hash - question - 1);
    std::wstring fragment = hash == std::wstring::npos ? L"" : url.substr(hash);
    std::wstring site = siteOf(url);
    bool siSite = site.find(L"spotify.com") != std::wstring::npos || site.find(L"youtube.com") != std::wstring::npos || site == L"youtu.be";

    std::wstring kept;
    bool removed = false;
    size_t start = 0;
    while (start <= query.size()) {
        size_t amp = query.find(L'&', start);
        std::wstring part = query.substr(start, amp == std::wstring::npos ? std::wstring::npos : amp - start);
        std::wstring name = util::urlDecode(part.substr(0, part.find(L'=')));
        bool tracking = !part.empty() && isTrackingParameter(name) && (lower(name) != L"si" || siSite);
        if (tracking)
            removed = true;
        else if (!part.empty())
            kept += (kept.empty() ? L"" : L"&") + part;
        if (amp == std::wstring::npos)
            break;
        start = amp + 1;
    }
    if (!removed)
        return url;
    return url.substr(0, question) + (kept.empty() ? L"" : L"?" + kept) + fragment;
}

std::wstring upgradeURL(const std::wstring& url)
{
    if (lower(url.substr(0, 7)) != L"http://")
        return { };
    std::wstring host = hostOf(url);
    if (isLocalHost(host) || g_httpThisSession.count(siteOf(url)) || !httpsFor(url))
        return { };
    return L"https://" + url.substr(7);
}

void allowHttpThisSession(const std::wstring& url)
{
    std::wstring site = siteOf(url);
    if (!site.empty())
        g_httpThisSession.insert(site);
}

std::string siteJSON(const std::wstring& url)
{
    return std::string("{\"https\":") + (httpsFor(url) ? "true" : "false")
        + ",\"params\":" + (trackingParamsFor(url) ? "true" : "false")
        + ",\"fingerprinting\":" + (fingerprintingFor(url) ? "true" : "false")
        + ",\"httpsMode\":" + util::jsonString(httpsMode()) + "}";
}

void setForSite(const std::wstring& url, const std::wstring& key, bool on)
{
    std::wstring site = siteOf(url);
    if (site.empty())
        return;
    auto& overrides = g_sites[site];
    // Back to the default when it matches the global setting.
    if (key == L"https") {
        overrides.https = on == (httpsMode() != L"off") ? -1 : on;
        if (on)
            g_httpThisSession.erase(site);
    } else if (key == L"params")
        overrides.params = on == globalOn(L"StripTrackingParams") ? -1 : on;
    else if (key == L"fingerprinting") {
        overrides.fingerprinting = on == globalOn(L"Fingerprinting") ? -1 : on;
        applyFingerprintScript();
    } else
        return;
    save();
}

std::string handleCommand(const std::wstring& command, const std::wstring& argument)
{
    if (command == L"list") {
        // [{"site": "...", "https": bool|null, "params": ..., "fingerprinting": ...}]
        std::string json = "[";
        bool first = true;
        for (auto& [site, overrides] : g_sites) {
            if (overrides.https < 0 && overrides.params < 0 && overrides.fingerprinting < 0)
                continue;
            auto value = [](int v) { return v < 0 ? std::string("null") : v ? std::string("true") : std::string("false"); };
            json += (first ? "" : ",") + std::string("{\"site\":") + util::jsonString(site) + ",\"https\":" + value(overrides.https)
                + ",\"params\":" + value(overrides.params) + ",\"fingerprinting\":" + value(overrides.fingerprinting) + "}";
            first = false;
        }
        return json + "]";
    }
    if (command == L"remove") {
        g_sites.erase(argument);
        g_httpThisSession.erase(argument);
        save();
        applyFingerprintScript();
        return "true";
    }
    return "null";
}

void settingsChanged()
{
    applyFingerprintScript();
}

} // namespace privacy
