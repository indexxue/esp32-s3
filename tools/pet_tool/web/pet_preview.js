/**
 * Product-synced round-screen preview logic (pet_core care tiers + chat D).
 */
(function () {
  const captions = {
    splash: ["开机 C", "身体 + 环形进度；Splash Gate ≥1s（预览可手动切页）。"],
    home: ["主界面", "Needs 三点 · Wi‑Fi · 左弧喂/玩/睡 · 右聊 · 点身体戳/长按喂。"],
    chat: ["对话 D", "进页只连会话；单击宠脸听↔等答；返回离开即停；字幕当前一轮。"],
  };

  const FEED_FULL = 85;
  const POKE_COLD = 30;
  const FEED_HUNGER = 28;
  const PLAY_MOOD = 24;
  const TAP_MOOD = 4;
  const PLAY_ENERGY = 8;

  const needs = { hunger: 70, mood: 70, energy: 80, sleeping: false };
  let page = "home";
  let holdTimer = null;
  let clipLockUntil = 0;
  let idleReturnTimer = null;
  let chatMode = "ready"; /* connecting | ready | listen | speak */

  const meta = document.getElementById("meta");
  const caption = document.getElementById("caption");
  const bezel = document.getElementById("bezel");
  const scale = document.getElementById("scale");
  const scaleVal = document.getElementById("scaleVal");
  const needsText = document.getElementById("needsText");
  const needsDots = document.getElementById("needsDots");
  const toast = document.getElementById("toast");
  const wifiIcon = document.getElementById("wifiIcon");
  const chatModeEl = document.getElementById("chatMode");
  const chatCap = document.getElementById("chatCap");
  const chatWave = document.getElementById("chatWave");

  function clamp(v) {
    return Math.max(0, Math.min(100, v | 0));
  }

  /** Mirror pet_core pick_idle_clip(): sleep_loop / sleepy / sad / idle. */
  function pickIdleClip() {
    if (needs.sleeping) return "sleep_loop";
    if (needs.energy < 25) return "sleepy";
    if (needs.hunger < 25) return "idle"; /* hungry face reserved; body stays idle */
    if (needs.mood < 25) return "sad";
    return "idle";
  }

  function restoreIdle() {
    idleReturnTimer = null;
    if (page === "chat") return;
    const id = pickIdleClip();
    if (window.PetSkinPreview) {
      PetSkinPreview.playClip(id);
    }
  }

  function showPage(id) {
    page = id;
    document.querySelectorAll(".page").forEach((p) => p.classList.toggle("on", p.id === id));
    document.querySelectorAll("[data-page]").forEach((b) => {
      b.classList.toggle("active", b.dataset.page === id);
    });
    const c = captions[id] || [id, ""];
    caption.innerHTML = "<strong>" + c[0] + "</strong> — " + c[1];
    meta.textContent = "page=" + id + " · 240×240 · product sync";
    if (id === "chat") {
      enterChat();
    } else if (chatMode !== "ready") {
      leaveChat(false);
    }
  }

  function renderNeeds() {
    needsText.textContent =
      "h=" + needs.hunger + " m=" + needs.mood + " e=" + needs.energy +
      (needs.sleeping ? " · sleep" : "");
    const dots = needsDots.querySelectorAll("b");
    const vals = [needs.hunger, needs.mood, needs.energy];
    dots.forEach((d, i) => d.classList.toggle("on", vals[i] >= 40));
  }

  function flashToast(msg) {
    if (!toast) return;
    toast.textContent = msg;
    toast.classList.add("show");
    setTimeout(() => toast.classList.remove("show"), 1600);
  }

  function playClip(id, lockMs) {
    const now = Date.now();
    if (now < clipLockUntil && id !== "refuse" && id !== "eat" && id !== "play") {
      return;
    }
    if (idleReturnTimer) {
      clearTimeout(idleReturnTimer);
      idleReturnTimer = null;
    }
    if (window.PetSkinPreview) {
      PetSkinPreview.playClip(id);
    }
    if (lockMs > 0) {
      clipLockUntil = now + lockMs;
      /* one-shot clips → restore ambient idle like pet_core tick */
      idleReturnTimer = setTimeout(restoreIdle, lockMs);
    } else {
      clipLockUntil = 0;
    }
  }

  function doWake() {
    if (!needs.sleeping) return;
    needs.sleeping = false;
    playClip("idle", 0);
    renderNeeds();
  }

  function doFeed() {
    doWake();
    if (needs.hunger >= FEED_FULL) {
      playClip("refuse", 2000);
      flashToast("full → refuse");
      return;
    }
    needs.hunger = clamp(needs.hunger + FEED_HUNGER);
    needs.mood = clamp(needs.mood + TAP_MOOD + 2);
    playClip("eat", 3000);
    renderNeeds();
  }

  function doPlay() {
    doWake();
    needs.mood = clamp(needs.mood + PLAY_MOOD);
    needs.energy = clamp(needs.energy - PLAY_ENERGY);
    playClip("play", 4000);
    renderNeeds();
  }

  function doSleep() {
    needs.sleeping = true;
    playClip("sleep_loop", 0);
    renderNeeds();
  }

  function doTap() {
    if (needs.sleeping) {
      doWake();
      flashToast("wake");
      return;
    }
    if (needs.mood < POKE_COLD) {
      playClip("refuse", 2000);
      flashToast("cold → refuse");
      return;
    }
    needs.mood = clamp(needs.mood + TAP_MOOD);
    playClip("poke", 1000);
    renderNeeds();
  }

  function setWifi(online) {
    wifiIcon.classList.toggle("off", !online);
    const img = wifiIcon.querySelector("img");
    if (img) {
      img.src = online ? "/skin/assets/ui_wifi_on.png" : "/skin/assets/ui_wifi_off.png";
    }
  }

  function tryThemeIcon(btn, asset, fallbackLetter) {
    const url = "/skin/assets/" + asset;
    const img = new Image();
    img.onload = () => {
      btn.textContent = "";
      let el = btn.querySelector("img");
      if (!el) {
        el = document.createElement("img");
        el.alt = "";
        btn.appendChild(el);
      }
      el.src = url;
    };
    img.onerror = () => {
      btn.textContent = fallbackLetter;
      const el = btn.querySelector("img");
      if (el) el.remove();
    };
    img.src = url;
  }

  function loadThemeUi() {
    tryThemeIcon(document.getElementById("careFeed"), "ui_feed.png", "F");
    tryThemeIcon(document.getElementById("carePlay"), "ui_play.png", "P");
    tryThemeIcon(document.getElementById("careSleep"), "ui_sleep.png", "S");
    tryThemeIcon(document.getElementById("careChat"), "ui_chat.png", "C");
    const online = document.getElementById("chkOnline").checked;
    wifiIcon.classList.add("fb");
    let wimg = wifiIcon.querySelector("img");
    if (!wimg) {
      wimg = document.createElement("img");
      wimg.alt = "";
      wifiIcon.appendChild(wimg);
    }
    const probe = new Image();
    probe.onload = () => {
      wifiIcon.classList.remove("fb");
      setWifi(online);
    };
    probe.onerror = () => {
      if (wimg) wimg.remove();
      wifiIcon.classList.add("fb");
      setWifi(online);
    };
    probe.src = online ? "/skin/assets/ui_wifi_on.png" : "/skin/assets/ui_wifi_off.png";
  }

  function enterChat() {
    chatMode = "connecting";
    applyChatMode();
    chatCap.textContent = "tap to talk";
    chatCap.classList.add("muted");
    setTimeout(() => {
      if (page !== "chat") return;
      chatMode = "ready";
      applyChatMode();
    }, 600);
  }

  function leaveChat(goHome) {
    chatMode = "ready";
    applyChatMode();
    if (goHome) showPage("home");
  }

  function applyChatMode() {
    chatModeEl.className = "mode";
    chatWave.classList.remove("on");
    if (chatMode === "listen") {
      chatModeEl.textContent = "listen";
      chatModeEl.classList.add("listen");
      chatWave.classList.add("on");
    } else if (chatMode === "speak") {
      chatModeEl.textContent = "speak";
      chatModeEl.classList.add("speak");
    } else if (chatMode === "connecting") {
      chatModeEl.textContent = "connecting";
      chatModeEl.classList.add("connecting");
    } else {
      chatModeEl.textContent = "ready";
    }
  }

  function toggleChatListen() {
    if (chatMode === "connecting") return;
    if (chatMode === "speak") {
      chatMode = "listen";
      chatCap.textContent = "listening…";
      chatCap.classList.remove("muted");
      applyChatMode();
      return;
    }
    if (chatMode === "listen") {
      chatMode = "speak";
      chatCap.textContent = "你好，我是桌宠。";
      chatCap.classList.remove("muted");
      applyChatMode();
      return;
    }
    chatMode = "listen";
    chatCap.textContent = "listening…";
    chatCap.classList.remove("muted");
    applyChatMode();
  }

  /* UI bind */
  document.querySelectorAll("[data-page]").forEach((b) => {
    b.addEventListener("click", () => showPage(b.dataset.page));
  });

  document.getElementById("careFeed").addEventListener("click", doFeed);
  document.getElementById("carePlay").addEventListener("click", doPlay);
  document.getElementById("careSleep").addEventListener("click", doSleep);
  document.getElementById("careChat").addEventListener("click", () => showPage("chat"));
  document.getElementById("chatBack").addEventListener("click", () => leaveChat(true));
  document.getElementById("chatBody").addEventListener("click", toggleChatListen);

  const homeBody = document.getElementById("homeBody");
  homeBody.addEventListener("pointerdown", () => {
    holdTimer = setTimeout(() => {
      holdTimer = null;
      doFeed();
    }, 650);
  });
  homeBody.addEventListener("pointerup", () => {
    if (holdTimer) {
      clearTimeout(holdTimer);
      holdTimer = null;
      doTap();
    }
  });
  homeBody.addEventListener("pointerleave", () => {
    if (holdTimer) {
      clearTimeout(holdTimer);
      holdTimer = null;
    }
  });

  document.getElementById("btnDecay").addEventListener("click", () => {
    if (needs.sleeping) {
      needs.hunger = clamp(needs.hunger - 1);
      needs.energy = clamp(needs.energy + 1);
    } else {
      needs.hunger = clamp(needs.hunger - 5);
      needs.mood = clamp(needs.mood - 5);
      needs.energy = clamp(needs.energy - 5);
    }
    renderNeeds();
  });
  document.getElementById("btnResetNeeds").addEventListener("click", () => {
    needs.hunger = 70;
    needs.mood = 70;
    needs.energy = 80;
    needs.sleeping = false;
    playClip("idle", 0);
    renderNeeds();
  });
  document.getElementById("chkOnline").addEventListener("change", (e) => {
    setWifi(e.target.checked);
  });

  function syncGuides() {
    document.getElementById("gCross").hidden = !document.getElementById("chkCross").checked;
    document.getElementById("gSafe").hidden = !document.getElementById("chkSafe").checked;
  }
  ["chkCross", "chkSafe"].forEach((id) => {
    document.getElementById(id).addEventListener("change", syncGuides);
  });
  syncGuides();

  function syncScale() {
    const v = Number(scale.value);
    scaleVal.textContent = v.toFixed(2).replace(/\.00$/, ".0") + "×";
    bezel.style.transform = "scale(" + v + ")";
    bezel.style.margin = (v - 1) * 120 + "px 0";
  }
  scale.addEventListener("input", syncScale);
  syncScale();

  window.addEventListener("keydown", (e) => {
    if (e.key === "1") showPage("splash");
    if (e.key === "2") showPage("home");
    if (e.key === "3") showPage("chat");
  });

  const origLoad = window.PetSkinPreview && window.PetSkinPreview.loadPack;
  if (window.PetSkinPreview) {
    PetSkinPreview.bindUi();
    const wrapped = async () => {
      const ok = await PetSkinPreview.loadPack();
      loadThemeUi();
      const splashPet = document.getElementById("splashPet");
      if (splashPet && ok) {
        /* splash image applied inside skin_loader */
      }
      return ok;
    };
    document.getElementById("skinReload").onclick = () => wrapped();
    wrapped();
  }

  renderNeeds();
  showPage("home");
})();
