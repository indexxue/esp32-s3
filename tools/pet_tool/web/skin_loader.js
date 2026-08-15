/**
 * Load tools/pet_tool/skin pack.json + PNG into round-screen preview.
 * Expects serve.py root so /skin/... is same-origin.
 */
(function (global) {
  const PACK_URL = "/skin/pack.json";
  const SKIN_BASE = "/skin/";

  let pack = null;
  let clips = {};
  let frameTimer = null;
  let frameIndex = 0;
  let currentClip = "idle";

  function resolveRel(rel) {
    if (!rel) return null;
    const s = String(rel).replace(/\\/g, "/");
    if (s.startsWith("http") || s.startsWith("/")) return s;
    return SKIN_BASE + s.replace(/^\.\//, "");
  }

  function clipSources(clip) {
    if (!clip) return [];
    if (Array.isArray(clip.sources) && clip.sources.length) {
      return clip.sources.map(resolveRel).filter(Boolean);
    }
    if (clip.source) {
      return [resolveRel(clip.source)].filter(Boolean);
    }
    return [];
  }

  function stopAnim() {
    if (frameTimer) {
      clearInterval(frameTimer);
      frameTimer = null;
    }
  }

  function applyUrl(url) {
    const targets = document.querySelectorAll("[data-skin-body]");
    targets.forEach((el) => {
      el.classList.toggle("has-skin", !!url);
      let img = el.querySelector("img.skin-frame");
      if (!url) {
        if (img) img.remove();
        el.style.backgroundImage = "";
        return;
      }
      if (!img) {
        img = document.createElement("img");
        img.className = "skin-frame";
        img.alt = "";
        img.draggable = false;
        el.appendChild(img);
      }
      if (img.getAttribute("src") !== url) {
        img.src = url;
      }
    });

    const splashPet = document.querySelector("#splashPet, #splash .pet, #splashC .pet");
    if (splashPet) {
      const splashUrl =
        (pack && pack.splash && pack.splash.source && resolveRel(pack.splash.source)) ||
        resolveRel("assets/splash.png") ||
        (clips.idle && clipSources(clips.idle)[0]) ||
        url;
      if (splashUrl) {
        splashPet.classList.add("has-skin");
        splashPet.style.backgroundImage = 'url("' + splashUrl + '")';
        splashPet.style.backgroundSize = "contain";
        splashPet.style.backgroundRepeat = "no-repeat";
        splashPet.style.backgroundPosition = "center";
      }
    }
  }

  function playClip(id) {
    currentClip = id || currentClip;
    let clip = clips[currentClip];
    let urls = clipSources(clip);
    /* firmware: refuse missing → sad frames */
    if (!urls.length && currentClip === "refuse" && clips.sad) {
      clip = clips.sad;
      urls = clipSources(clip);
    }
    stopAnim();
    frameIndex = 0;
    if (!urls.length) {
      applyUrl(null);
      setStatus("clip '" + currentClip + "' has no PNG (fallback CSS)");
      return;
    }
    applyUrl(urls[0]);
    const fps = Math.max(1, Number(clip.fps) || 4);
    if (urls.length > 1) {
      frameTimer = setInterval(() => {
        frameIndex = (frameIndex + 1) % urls.length;
        applyUrl(urls[frameIndex]);
      }, Math.round(1000 / fps));
    }
    setStatus("clip=" + currentClip + " · frames=" + urls.length + " · " + fps + "fps");
    const sel = document.getElementById("skinClip");
    if (sel && sel.value !== currentClip) {
      /* keep select in sync when possible */
      if ([].some.call(sel.options, (o) => o.value === currentClip)) {
        sel.value = currentClip;
      }
    }
  }

  function setStatus(text) {
    const el = document.getElementById("skinStatus");
    if (el) el.textContent = text;
  }

  function fillClipSelect() {
    const sel = document.getElementById("skinClip");
    if (!sel) return;
    sel.innerHTML = "";
    Object.keys(clips).forEach((id) => {
      const opt = document.createElement("option");
      opt.value = id;
      opt.textContent = id + " (" + clipSources(clips[id]).length + ")";
      sel.appendChild(opt);
    });
    if (clips[currentClip]) sel.value = currentClip;
  }

  async function loadPack() {
    setStatus("loading pack.json…");
    stopAnim();
    try {
      const res = await fetch(PACK_URL + "?t=" + Date.now(), { cache: "no-store" });
      if (!res.ok) throw new Error("HTTP " + res.status);
      pack = await res.json();
      clips = {};
      (pack.clips || []).forEach((c) => {
        if (c && c.id) clips[c.id] = c;
      });
      fillClipSelect();
      if (!clips[currentClip]) {
        currentClip = Object.keys(clips)[0] || "idle";
      }
      playClip(currentClip);
      const toast = document.querySelector("#home2 .toast");
      if (toast) {
        toast.textContent = "";
        toast.classList.remove("show");
      }
      return true;
    } catch (err) {
      pack = null;
      clips = {};
      applyUrl(null);
      setStatus("pack load failed — use serve.py (not file://). " + err);
      return false;
    }
  }

  function bindUi() {
    const reload = document.getElementById("skinReload");
    if (reload) reload.addEventListener("click", () => loadPack());
    const sel = document.getElementById("skinClip");
    if (sel) {
      sel.addEventListener("change", () => playClip(sel.value));
    }
  }

  global.PetSkinPreview = {
    loadPack,
    playClip,
    bindUi,
    getClip: () => currentClip,
  };
})(window);
