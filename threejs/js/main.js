/* =========================================================
 * main.js — 渲染器 / 相机轨道 / 全局光照 / 组装场景
 * ========================================================= */
'use strict';
(function (App) {

  App.boot = function () {
    /* ---------- 渲染器 ---------- */
    let renderer;
    try {
      renderer = new THREE.WebGLRenderer({ antialias: true, alpha: false, logarithmicDepthBuffer: true });
    } catch (e) {
      document.body.innerHTML = '<p style="color:#889;font:14px sans-serif;padding:2em">此浏览器不支持 WebGL。</p>';
      return;
    }
    renderer.setPixelRatio(Math.min(window.devicePixelRatio || 1, 2));
    renderer.setSize(window.innerWidth, window.innerHeight);
    renderer.outputEncoding = THREE.sRGBEncoding;
    renderer.toneMapping = THREE.ACESFilmicToneMapping;
    renderer.toneMappingExposure = 1.06;
    renderer.shadowMap.enabled = true;
    renderer.shadowMap.type = THREE.PCFSoftShadowMap;
    document.body.appendChild(renderer.domElement);
    renderer.domElement.style.cursor = 'grab';
    renderer.domElement.addEventListener('contextmenu', (e) => e.preventDefault());

    /* ---------- 场景 ---------- */
    const scene = new THREE.Scene();
    scene.background = new THREE.Color('#070a13');
    // 远程模式是默认入口（展示形态）：?local=1 才进本地便利店 demo。
    // 服务端地址缺省 <页面host>:9151（demo.sh 的默认端口），token 缺省 devtoken。
    const remoteParams = new URLSearchParams(location.search);
    const remoteMode = !remoteParams.get('local');
    let remoteBase;
    if (remoteMode) {
      remoteBase = remoteParams.get('remote') ||
        `${location.protocol}//${location.hostname || '127.0.0.1'}:9151`;
    }

    if (!remoteMode) {
    // 夜空渐变穹顶
    const skyMat = new THREE.ShaderMaterial({
      side: THREE.BackSide,
      depthWrite: false,
      uniforms: {},
      vertexShader: `
        varying vec3 vP;
        void main() {
          vP = position;
          gl_Position = projectionMatrix * modelViewMatrix * vec4(position, 1.0);
        }`,
      fragmentShader: `
        varying vec3 vP;
        void main() {
          float h = normalize(vP).y * 0.5 + 0.5;
          vec3 top = vec3(0.055, 0.075, 0.14);
          vec3 mid = vec3(0.10, 0.13, 0.22);
          vec3 bot = vec3(0.028, 0.032, 0.055);
          vec3 c = h > 0.5 ? mix(mid, top, (h - 0.5) * 2.0) : mix(bot, mid, h * 2.0);
          // 地平线城市光晕（偏暖）
          float glow = exp(-abs(normalize(vP).y) * 7.0) * 0.35;
          c += vec3(0.16, 0.11, 0.07) * glow;
          gl_FragColor = vec4(c, 1.0);
        }`,
    });
    const sky = new THREE.Mesh(new THREE.SphereGeometry(70, 24, 16), skyMat);
    scene.add(sky);

    // 稀疏星点
    const starGeo = new THREE.BufferGeometry();
    const starPos = [];
    for (let i = 0; i < 240; i++) {
      const th = Math.random() * Math.PI * 2;
      const ph = Math.acos(rand(0.1, 0.95));
      const r = 62;
      starPos.push(r * Math.sin(ph) * Math.cos(th), r * Math.cos(ph), r * Math.sin(ph) * Math.sin(th));
    }
    function rand(a, b) { return a + Math.random() * (b - a); }
    starGeo.setAttribute('position', new THREE.Float32BufferAttribute(starPos, 3));
    const stars = new THREE.Points(starGeo, new THREE.PointsMaterial({
      color: '#c8d4ee', size: 0.14, sizeAttenuation: true, transparent: true, opacity: 0.55,
    }));
    scene.add(stars);
    }


    /* ---------- 相机 + 轨道 ---------- */
    const camera = new THREE.PerspectiveCamera(42, window.innerWidth / window.innerHeight, 0.3, 200);
    camera.position.set(7.6, 6.4, 11.8);
    // 调试：URL ?cam=x,y,z 可覆盖初始机位
    const camParam = new URLSearchParams(location.search).get('cam');
    if (camParam) {
      const v = camParam.split(',').map(Number);
      if (v.length === 3 && v.every(Number.isFinite)) camera.position.set(v[0], v[1], v[2]);
    }
    const controls = new THREE.OrbitControls(camera, renderer.domElement);
    controls.target.set(0, 0.85, 0);
    controls.enableDamping = true;
    controls.dampingFactor = 0.07;
    controls.rotateSpeed = 0.75;
    controls.zoomSpeed = 0.9;
    controls.panSpeed = 0.6;
    controls.screenSpacePanning = true;
    controls.minDistance = 4.2;
    controls.maxDistance = 28;
    controls.minPolarAngle = 0.12;
    controls.maxPolarAngle = 1.46;
    if (remoteMode) {
      // 远程模式放宽距离钳制：minDistance 4.2 会把 session-ready 同步的
      // targetDist=4 机位沿视线推出 0.2m（实测把 Bistro 内景标定机位推进
      // 墙体 → 全黑）；maxDistance 28 也限制不了从室内穿窗走到街面的行程。
      controls.minDistance = 0.1;
      controls.maxDistance = 80;
    }
    const homeTarget = new THREE.Vector3(0, 0.85, 0);
    const targetClamp = new THREE.Vector3();

    /* ---------- 远程渲染控制 ---------- */
    const remote = remoteMode ? createRemoteClient(remoteBase, camera, controls) : null;

    if (!remoteMode) {
    /* ---------- 全局光照 ---------- */
    const hemi = new THREE.HemisphereLight('#2e3d5f', '#0e1119', 0.28);
    scene.add(hemi);
    const moon = new THREE.DirectionalLight('#8fa8e0', 0.42);
    moon.position.set(-7, 10, -5);
    moon.castShadow = true;
    moon.shadow.mapSize.set(2048, 2048);
    moon.shadow.camera.left = -7.5;
    moon.shadow.camera.right = 7.5;
    moon.shadow.camera.top = 7.5;
    moon.shadow.camera.bottom = -7.5;
    moon.shadow.camera.near = 2;
    moon.shadow.camera.far = 26;
    moon.shadow.bias = -0.0006;
    moon.shadow.normalBias = 0.02;
    scene.add(moon);
    scene.add(moon.target);
    // 正面微弱冷补光，避免背光死黑
    const fill = new THREE.DirectionalLight('#4a5c86', 0.12);
    fill.position.set(6, 4, 8);
    scene.add(fill);

    /* ---------- 组装 ---------- */
    const streetRefs = App.buildStreet(scene);
    const storeRefs = App.buildStore(scene);
    App.buildCars(scene);
    const fx = App.buildEffects(scene, storeRefs, streetRefs);
    }


    /* ---------- 循环 ---------- */
    const clock = new THREE.Clock();
    function loop() {
      requestAnimationFrame(loop);
      const dt = Math.min(clock.getDelta(), 0.05);
      const t = clock.elapsedTime;
      controls.update();
      if (!remoteMode) {
        // 目标点限制在底座中心附近
        targetClamp.copy(controls.target).sub(homeTarget);
        if (targetClamp.length() > 2.4) {
          targetClamp.setLength(2.4);
          controls.target.copy(homeTarget).add(targetClamp);
        }
        fx.update(dt, t);
        renderer.render(scene, camera);
      }
      // remote 模式：画面全在 overlay，本地空场景的 renderer.render 每帧
      // 纯属浪费——controls.update() 仍跑（阻尼语义依赖它）。
    }
    loop();

    window.addEventListener('resize', () => {
      camera.aspect = window.innerWidth / window.innerHeight;
      camera.updateProjectionMatrix();
      renderer.setSize(window.innerWidth, window.innerHeight);
    });
  };

  function createRemoteClient(baseUrl, camera, controls) {
    const base = baseUrl.replace(/\/$/, '');
    const params = new URLSearchParams(location.search);
    // 展示默认：合并内外景（?model= 可切 bistro-interior / sphere-test），
    // P1 场景切换器运行时可改（同步回 URL）
    let model = params.get('model') || 'bistro';
    // 访问 token（设计 13）：服务端启动时生成/由 RENDER_TOKEN 指定，
    // 通过页面 URL ?token= 传入，附加到所有 HTTP/WS/图片请求。
    // 展示缺省 devtoken（demo.sh 的默认值）。
    const token = params.get('token') || 'devtoken';
    const dragMs = Math.max(60, parseInt(params.get('dragMs') || '120', 10) || 120);
    const idleMs = Math.max(150, parseInt(params.get('idleMs') || '400', 10) || 400);
    // 设计 17 的两档画质：拖动预览低分辨率低 spp 快照，停止后渐进精修。
    // 拖动档 8spp：2spp 在昏暗场景（合并内外景的室内）几乎全黑，8spp 在
    // 320×180 下仍然即时。IDLE 档走渐进链（progressive:1，几何 chunk 计划
    // 2,4,8,16,...），分辨率默认 960×540（&idleRes=1280x720 可调）。
    // NEE K 维持服务端默认（K 是全局符号，跨 session 配置有竞争，暂不开放）。
    const DRAG = { width: 320, height: 180, sppBudget: 8, maxDepth: 4,
                   neeK: 1, denoise: 1, progressive: 1 };
    const idleRes = (params.get('idleRes') || '960x540')
      .split('x').map(v => Math.max(16, parseInt(v, 10) || 0));
    const IDLE = { width: idleRes[0] || 960, height: idleRes[1] || 540,
                   sppBudget: 128, maxDepth: 6, neeK: 2, denoise: 1, progressive: 1,
                   adaptiveMin: 16, adaptiveCheck: 8, adaptiveThresh: 0.02 };
    // 页面可覆盖显示曝光（&exposure=1.75）；缺省跟随 session preset
    const exposure = Math.max(0, parseFloat(params.get('exposure')) || 0);
    if (exposure > 0) { DRAG.exposure = exposure; IDLE.exposure = exposure; }
    // 室内自动提亮档（bistro 合并场景）：session-ready 时从服务端读
    // indoorExposure/indoorMin/Max/Margin，之后每次提交按相机位置在
    // 室外档(base)与室内档之间做 smoothstep 混合（display-only，不重置
    // 累积）。&exposure= 手动覆盖时禁用自动档。
    let autoExposure = null;
    // 运行时手动/自动（P0 曝光滑条）：autoOn=false 时用滑条值；URL &exposure=
    // 视为初始手动值并关闭自动档
    let manualExp = exposure > 0 ? exposure : 0;
    let autoOn = !(exposure > 0);
    // ---- B1 相机：FOV 与景深（?fov= &coc=；coc>0 即开启 DOF，对焦距离
    // 每次提交自动取轨道目标距离）----
    const urlFov = Math.min(140, Math.max(10, parseFloat(params.get('fov')) || 0));
    let fovTouched = urlFov > 0;   // 用户是否改过 FOV（URL 写回与 preset 覆盖用）
    let dofCoc = Math.min(6, Math.max(0.5, parseFloat(params.get('coc')) || 0)) || 0;
    // ---- B2 画面（display-only，服务端不重置累积）：?sat= &tint= &bloom= ----
    // *Touched=false / bloom=-1 = 未设置（跟随 session preset 默认）
    let satTouched = params.has('sat');
    let dispSat = Math.min(2, Math.max(0, parseFloat(params.get('sat')) || 1));
    let tintTouched = params.has('tint');
    let dispTint = Math.min(2, Math.max(0.5, parseFloat(params.get('tint')) || 1));
    let dispBloom = params.has('bloom') ? (params.get('bloom') === '1' ? 1 : 0) : -1;
    // ---- B3 太阳（radiance 变化，服务端重置累积重染）：?sun=az,el,int ----
    let sun = null;
    if (params.get('sun')) {
      const s = params.get('sun').split(',').map(Number);
      if (s.length === 3 && s.every(v => isFinite(v)))
        sun = { az: Math.max(0, Math.min(360, s[0])),
                el: Math.max(0, Math.min(90, s[1])),
                int: Math.max(0, Math.min(100, s[2])) };
    }
    // ---- B4 暂停精修：停手后不再自动触发精修链 ----
    let refinePaused = false;
    function tierExposure(pos) {
      if (!autoOn && manualExp > 0) return manualExp;    // 运行时手动（滑条）
      if (exposure > 0) return exposure;                 // URL 手动覆盖
      if (!autoExposure) return 0;                       // 同步前不发送
      if (!autoExposure.indoor) return 0;                // 无室内档：用 session 默认
      const p = [pos.x, pos.y, pos.z];                   // Vector3 没有下标访问
      let d2 = 0;
      for (let a = 0; a < 3; ++a) {
        const over = Math.max(autoExposure.min[a] - p[a],
                              p[a] - autoExposure.max[a], 0);
        d2 += over * over;
      }
      const d = Math.sqrt(d2);
      const m = Math.max(0.25, autoExposure.margin || 1.5);
      const u = Math.min(1, Math.max(0, 1 - d / m));
      const s = u * u * (3 - 2 * u);                     // smoothstep
      return autoExposure.base + (autoExposure.indoor - autoExposure.base) * s;
    }

    let sessionId = null;
    let serverReady = false;
    let generation = 0;
    let socketWanted = false;
    let sock = null;                 // WS 句柄（P1 场景切换时需要关闭旧连接）
    let connectAttempts = 0;
    // 旧帧丢弃（设计 8）：renderId + generation + sequence 三元组
    let submittedRenderId = null;
    let shownRenderId = null;
    let shownSequence = 0;
    let overlayObjUrl = null;   // B5：当前 overlay 的 blob: URL（换帧时 revoke）
    // P1 统计面板数据源：有效 spp / chunk 间隔（预览事件到达间隔中位数）
    let lastSample = 0, lastBudget = 0, lastPrevT = 0;
    const cadence = [];
    // P2-11 加载进度：阶段权重（合并场景 parse/decode 各两轮 → 进度单调不降）
    const LOAD_STAGES = { parse: [0, .2], decode: [.2, .6], geometry: [.6, .72],
                          materials: [.72, .78], upload: [.78, .97], sky: [.97, 1] };
    let loadFrac = 0;
    // P2-12 ETA：样本历史（相邻预览的样本增量 ≈ 当前 chunk 大小）
    const sampleHist = [];
    // P2-13 A/B 对比：当前链首帧 + 对比模式状态
    let chainFirstUrl = null, chainFirstSample = 0;
    let abOn = false, abPct = 50;
    // 相机提交状态机
    let dragTimer = null;
    let idleTimer = null;
    let postInFlight = false;
    let dirty = false;

    const overlay = document.createElement('img');
    overlay.alt = '';
    overlay.hidden = true;
    overlay.style.cssText = 'position:fixed;inset:0;width:100%;height:100%;object-fit:contain;pointer-events:none;z-index:10;transition:filter .25s ease-out;';
    document.body.appendChild(overlay);

    // ---- 展示 UI 包装（批：可展示性）----
    const style = document.createElement('style');
    style.textContent = `
      .re-chip { position:fixed; z-index:11; pointer-events:none; color:#e2e8f0;
        background:rgba(5,7,12,.72); border-radius:6px; backdrop-filter:blur(2px); }
      .re-title { top:12px; left:12px; padding:8px 12px; font:600 14px/1.3 system-ui,sans-serif; }
      .re-title small { display:block; font:400 11px/1.4 system-ui,sans-serif; color:#94a3b8; }
      .re-hint { bottom:14px; left:50%; transform:translateX(-50%); padding:6px 12px;
        font:12px system-ui,sans-serif; color:#94a3b8; transition:opacity 1.2s; }
      .re-status { left:12px; bottom:12px; padding:8px 10px 7px; width:250px;
        font:12px monospace; }
      .re-bar { height:3px; margin-bottom:6px; background:rgba(148,163,184,.25);
        border-radius:2px; overflow:hidden; }
      .re-bar i { display:block; height:100%; width:0%; background:#60a5fa;
        border-radius:2px; transition:width .3s; }
      .re-bar.indet i { width:35% !important; animation:re-slide 1.1s ease-in-out infinite; }
      @keyframes re-slide { 0%{margin-left:-35%} 100%{margin-left:100%} }
      .re-bar.done { opacity:0; transition:opacity .8s; }
      .re-ctl { position:fixed; top:12px; right:12px; z-index:11; display:flex;
        flex-direction:column; gap:6px; }
      .re-btn { width:34px; height:34px; border-radius:6px; background:rgba(5,7,12,.72);
        border:1px solid rgba(148,163,184,.25); color:#e2e8f0; cursor:pointer;
        display:flex; align-items:center; justify-content:center; padding:0; }
      .re-btn:hover { background:rgba(15,23,42,.88); border-color:rgba(148,163,184,.55); }
      .re-btn svg { width:17px; height:17px; stroke:#e2e8f0; fill:none;
        stroke-width:1.8; stroke-linecap:round; stroke-linejoin:round; }
      .re-panel { position:fixed; top:52px; right:12px; z-index:11; width:228px;
        padding:10px 12px 12px; border-radius:6px; background:rgba(5,7,12,.85);
        border:1px solid rgba(148,163,184,.25); color:#e2e8f0;
        font:12px system-ui,sans-serif; max-height:calc(100vh - 70px);
        overflow-y:auto; }
      .re-panel::-webkit-scrollbar { width:6px; }
      .re-panel::-webkit-scrollbar-thumb { background:rgba(148,163,184,.4);
        border-radius:3px; }
      .re-panel h4 { margin:2px 0 7px; font-size:11px; color:#94a3b8;
        font-weight:600; letter-spacing:.05em; text-transform:uppercase; }
      .re-q { display:flex; gap:4px; flex-wrap:wrap; }
      .re-q button { flex:1 1 44%; padding:5px 4px; font:11px system-ui,sans-serif;
        color:#cbd5e1; background:rgba(148,163,184,.12); border:1px solid transparent;
        border-radius:4px; cursor:pointer; }
      .re-q button.on { background:rgba(96,165,250,.25); color:#fff;
        border-color:rgba(96,165,250,.55); }
      .re-row { display:flex; align-items:center; gap:8px; margin-top:11px; }
      .re-row input[type=range] { flex:1; accent-color:#60a5fa; min-width:0; }
      .re-row .re-ev { min-width:28px; text-align:right; font-family:monospace; }
      .re-row label { display:flex; gap:4px; align-items:center; color:#cbd5e1;
        cursor:pointer; white-space:nowrap; }
      .re-row input[type=checkbox] { accent-color:#60a5fa; }
      .re-err { position:fixed; inset:0; z-index:12; display:flex; align-items:center;
        justify-content:center; background:rgba(4,6,10,.55); }
      .re-err[hidden] { display:none; }
      .re-err .card { background:rgba(10,14,22,.95); border:1px solid rgba(148,163,184,.35);
        border-radius:10px; padding:22px 26px; max-width:420px; text-align:center;
        font:14px/1.7 system-ui,sans-serif; color:#e2e8f0; }
      .re-err .card code { display:block; margin:10px 0 14px; padding:8px 10px;
        background:rgba(148,163,184,.12); border-radius:5px; font:12px monospace;
        color:#93c5fd; }
      .re-err .card button { padding:7px 18px; border-radius:5px; cursor:pointer;
        border:1px solid rgba(96,165,250,.5); background:rgba(96,165,250,.2);
        color:#e2e8f0; font:13px system-ui,sans-serif; }
      .re-err .card button:hover { background:rgba(96,165,250,.35); }
      .re-hideui .re-chip, .re-hideui .re-ctl, .re-hideui .re-panel { display:none; }
      .re-ab { position:fixed; inset:0; width:100%; height:100%; object-fit:contain;
        pointer-events:none; z-index:10; }
      .re-ab-div { position:fixed; top:0; bottom:0; width:2px; background:#60a5fa;
        z-index:11; cursor:ew-resize; }
      .re-ab-div::after { content:''; position:absolute; top:50%; left:-8px;
        width:18px; height:18px; margin-top:-9px; border-radius:50%;
        background:#60a5fa; box-shadow:0 0 6px rgba(0,0,0,.6); }
      .re-ab-tag { position:fixed; top:60px; z-index:11; padding:4px 8px;
        font:11px monospace; background:rgba(5,7,12,.72); color:#e2e8f0;
        border-radius:4px; pointer-events:none; }
      @media (pointer: coarse) {
        .re-chip { font-size:13px; }
        .re-title { padding:10px 14px; }
        .re-btn { width:44px; height:44px; }
        .re-btn svg { width:20px; height:20px; }
        .re-panel { width:262px; }
        .re-status { width:300px; }
      }`;
    document.head.appendChild(style);

    const title = document.createElement('div');
    title.className = 're-chip re-title';
    title.innerHTML = 'Raypr3sso <small>CUDA path tracing · remote progressive viewer</small>';
    document.body.appendChild(title);

    const hint = document.createElement('div');
    hint.className = 're-chip re-hint';
    hint.textContent = '拖动 环视 · 滚轮 进出建筑 · 停手自动精修';
    document.body.appendChild(hint);
    setTimeout(() => { hint.style.opacity = '0'; }, 9000);

    const status = document.createElement('div');
    status.className = 're-chip re-status';
    status.innerHTML = '<div class="re-bar indet"><i></i></div><span>remote: connecting</span>';
    document.body.appendChild(status);
    const bar = status.querySelector('.re-bar');
    function setBar(fraction) {
      // fraction 0..1 → 确定性收敛进度；null → 不确定动画（场景加载中）
      if (fraction == null) { bar.className = 're-bar indet'; bar.querySelector('i').style.width = ''; }
      else {
        bar.className = 're-bar' + (fraction >= 1 ? ' done' : '');
        bar.querySelector('i').style.width = (Math.min(1, fraction) * 100).toFixed(1) + '%';
      }
    }
    function show(text) {
      status.querySelector('span').textContent = 'remote: ' + text;
      // 依据状态文本切换加载/收敛条形态（保持 'remote: ' 前缀供 E2E 探针）
      if (/loading|connecting|unavailable/.test(text)) setBar(null);
    }

    // ---- 控制面板（P0：保存帧 / 回位 / 质量档 / 曝光 / 全屏；B1-B4：相机 /
    // 画面 / 太阳 / 暂停精修）----
    const SVG = {
      settings: '<svg viewBox="0 0 24 24"><line x1="4" y1="6" x2="20" y2="6"/><circle cx="14" cy="6" r="2.2"/><line x1="4" y1="12" x2="20" y2="12"/><circle cx="8" cy="12" r="2.2"/><line x1="4" y1="18" x2="20" y2="18"/><circle cx="16" cy="18" r="2.2"/></svg>',
      save: '<svg viewBox="0 0 24 24"><path d="M12 4v10m0 0l-4-4m4 4l4-4M5 19h14"/></svg>',
      home: '<svg viewBox="0 0 24 24"><path d="M4 11l8-7 8 7M6 10v9h12v-9"/></svg>',
      full: '<svg viewBox="0 0 24 24"><path d="M4 9V4h5M15 4h5v5M20 15v5h-5M9 20H4v-5"/></svg>',
      pause: '<svg viewBox="0 0 24 24"><path d="M9 5v14M15 5v14"/></svg>',
      play: '<svg viewBox="0 0 24 24"><path d="M8 5l11 7-11 7z"/></svg>'
    };
    const ctl = document.createElement('div');
    ctl.className = 're-ctl';
    const mkBtn = (id, icon, title, fn) => {
      const b = document.createElement('button');
      b.id = 're-btn-' + id;
      b.className = 're-btn'; b.title = title; b.innerHTML = icon;
      b.addEventListener('click', fn);
      ctl.appendChild(b);
      return b;
    };
    const panel = document.createElement('div');
    panel.className = 're-panel';
    panel.hidden = true;

    let saving = false;
    function saveFrame() {
      // 无损 PNG（服务端 /image = 精修终帧）；页面与服务器跨端口，用
      // fetch+blob 下载（a[download] 对跨源 URL 会被浏览器忽略）。
      if (!submittedRenderId || saving) return;
      saving = true;
      fetch(endpoint(authPath(`/api/renders/${submittedRenderId}/image`)))
        .then(r => { if (!r.ok) throw new Error(r.status); return r.blob(); })
        .then(b => {
          const a = document.createElement('a');
          a.href = URL.createObjectURL(b);
          a.download = `raypr3sso_${submittedRenderId}.png`;
          document.body.appendChild(a); a.click(); a.remove();
          setTimeout(() => URL.revokeObjectURL(a.href), 5000);
        })
        .catch(() => show('保存失败'))
        .finally(() => { saving = false; });
    }

    // ---- 模糊→清晰（反馈：移动视角后由模糊变清晰，而非黑屏到出现）----
    // 拖动档帧固定 4px 模糊（低 spp 的"大概"观感）；精修帧随 spp 5→0px；
    // 相机一开始移动就把当前（旧）帧糊 4px——旧帧全程保留，不出现黑屏。
    function applyBlur(sample, budget) {
      if (abOn) { overlay.style.filter = ''; return; }
      const frac = budget > 0 ? sample / budget : 1;
      const blurPx = budget <= 16 ? 4 : 5 * (1 - Math.min(1, frac));
      overlay.style.filter = `blur(${Math.max(0, blurPx).toFixed(1)}px)`;
    }
    let presetCam = null;   // session-ready 时的标定机位（服务端 preset）
    function applySessionCamera(info) {
      const c = info.camera;
      const pos = new THREE.Vector3(c.position[0], c.position[1], c.position[2]);
      const dir = new THREE.Vector3(c.direction[0], c.direction[1], c.direction[2]).normalize();
      const maxSyncDist = Math.max(1, parseFloat(params.get('targetDist')) || 4);
      const dist = Math.min(
        Math.max(1, camera.position.distanceTo(controls.target)), maxSyncDist);
      camera.position.copy(pos);
      controls.target.copy(pos.clone().add(dir.multiplyScalar(dist)));
      camera.fov = c.fov;
      camera.updateProjectionMatrix();
      controls.update();
      presetCam = c;
    }
    function goHome() {
      if (!presetCam) return;
      applySessionCamera({ camera: presetCam });
      setBar(0);
      show(model + ' ready');
      scheduleDrag();
      scheduleIdle();
    }
    function toggleFullscreen() {
      document.fullscreenElement
        ? document.exitFullscreen()
        : document.documentElement.requestFullscreen();
    }

    // 质量档：改 IDLE 分辨率并立即触发一次精修（服务端在分辨率变化时
    // 自动重置累积，无需刷新页面）。
    const QUALITIES = [
      { name: '流畅', w: 640, h: 360 },
      { name: '标准', w: 960, h: 540 },
      { name: '高清', w: 1280, h: 720 },
      { name: '超清', w: 1920, h: 1080 }
    ];
    let qualityIdx = QUALITIES.findIndex(
      q => q.w === IDLE.width && q.h === IDLE.height);
    if (qualityIdx < 0) qualityIdx = 1;
    function setQuality(i) {
      qualityIdx = i;
      IDLE.width = QUALITIES[i].w;
      IDLE.height = QUALITIES[i].h;
      syncUrl();
      panel.querySelectorAll('.re-q button')
        .forEach((b, j) => b.classList.toggle('on', j === i));
      updateStats();
      if (serverReady) submitRender(IDLE);
    }

    mkBtn('settings', SVG.settings, '质量 / 曝光', () => { panel.hidden = !panel.hidden; });
    mkBtn('save', SVG.save, '保存当前帧 (PNG)', saveFrame);
    mkBtn('home', SVG.home, '回到标定机位 (R)', goHome);
    mkBtn('full', SVG.full, '全屏 (F)', toggleFullscreen);
    document.body.appendChild(ctl);

    // 设置面板内容。注意：曝光滑条必须是面板里第一个 input[type=range]
    // （E2E 脚本以 .re-panel input[type=range] 定位它）。
    const rowHtml = (label, input, valId) =>
      `<div class="re-row"><span>${label}</span>${input}` +
      (valId ? `<span class="re-ev" id="${valId}"></span>` : '') + `</div>`;
    const rangeHtml = (id, min, max, step, val) =>
      `<input type="range" id="${id}" min="${min}" max="${max}" step="${step}" value="${val}">`;
    panel.innerHTML = '<h4>精修分辨率</h4><div class="re-q"></div>'
      + rowHtml('曝光',
          rangeHtml('re-in-exp', 0.5, 8, 0.1, (exposure > 0 ? exposure : 2.5).toFixed(1)),
          're-v-exp')
      + `<div class="re-row"><label title="室内自动提亮档按相机位置混合；手动时用滑条值">`
        + `<input type="checkbox" id="re-in-expauto" ${exposure > 0 ? '' : 'checked'}>自动</label></div>`
      + '<h4>画面</h4>'
      + `<div class="re-row"><label title="亮部溢光（display-only，即时生效）">`
        + `<input type="checkbox" id="re-in-bloom">Bloom 溢光</label></div>`
      + rowHtml('饱和', rangeHtml('re-in-sat', 0, 2, 0.05, dispSat > 0 ? dispSat : 1), 're-v-sat')
      + rowHtml('暖色', rangeHtml('re-in-tint', 0.5, 2, 0.05, dispTint > 0 ? dispTint : 1), 're-v-tint')
      + '<h4>相机</h4>'
      + rowHtml('FOV', rangeHtml('re-in-fov', 20, 100, 1, 45), 're-v-fov')
      + `<div class="re-row"><label title="景深：对焦距离自动取观察目标，光圈控制虚化强度">`
        + `<input type="checkbox" id="re-in-dof">景深</label></div>`
      + rowHtml('光圈', rangeHtml('re-in-coc', 0.5, 6, 0.1, dofCoc > 0 ? dofCoc : 2), 're-v-coc')
      + '<h4>太阳</h4>'
      + rowHtml('方位', rangeHtml('re-in-sunaz', 0, 360, 1, sun ? sun.az : 90), 're-v-sunaz')
      + rowHtml('高度', rangeHtml('re-in-sunel', 0, 90, 1, sun ? sun.el : 12), 're-v-sunel')
      + rowHtml('强度', rangeHtml('re-in-sunint', 0, 100, 1, sun ? sun.int : 85), 're-v-sunint');
    document.body.appendChild(panel);
    const qRow = panel.querySelector('.re-q');
    QUALITIES.forEach((q, j) => {
      const b = document.createElement('button');
      b.textContent = q.name;
      b.classList.toggle('on', j === qualityIdx);
      b.addEventListener('click', () => setQuality(j));
      qRow.appendChild(b);
    });
    const expSlider = document.getElementById('re-in-exp');
    const expLabel = document.getElementById('re-v-exp');
    const expAuto = document.getElementById('re-in-expauto');
    expLabel.textContent = (exposure > 0 ? exposure : 2.5).toFixed(1);
    let expTimer = null;
    expSlider.addEventListener('input', () => {
      autoOn = false;                       // 拖滑条 = 切手动
      manualExp = parseFloat(expSlider.value);
      expLabel.textContent = manualExp.toFixed(1);
      expAuto.checked = false;
      // 防抖：拖动过程 input 高频触发，停手 250ms 后才提交一次精修
      if (expTimer) clearTimeout(expTimer);
      expTimer = setTimeout(() => { syncUrl(); if (serverReady) submitRender(IDLE); }, 250);
    });
    expAuto.addEventListener('change', () => {
      autoOn = expAuto.checked;
      if (!autoOn) {
        manualExp = parseFloat(expSlider.value);
        if (serverReady) submitRender(IDLE);
      } else if (serverReady) {
        submitRender(IDLE);                 // 回到自动档（室内提亮恢复）
      }
      syncUrl();
    });

    // ---- B1 相机：FOV / 景深 ----
    // FOV 拖动中走拖动档（即时预览），释放后精修——与视角拖动同一条路径。
    const fovSlider = document.getElementById('re-in-fov');
    const fovVal = document.getElementById('re-v-fov');
    fovVal.textContent = fovSlider.value + '°';
    fovSlider.addEventListener('input', () => {
      fovTouched = true;
      camera.fov = parseFloat(fovSlider.value);
      camera.updateProjectionMatrix();
      fovVal.textContent = fovSlider.value + '°';
      if (serverReady) scheduleDrag();
    });
    fovSlider.addEventListener('change', () => { syncUrl(); if (serverReady) scheduleIdle(); });
    const dofBox = document.getElementById('re-in-dof');
    const cocSlider = document.getElementById('re-in-coc');
    const cocVal = document.getElementById('re-v-coc');
    const syncDofUi = () => {
      dofBox.checked = dofCoc > 0;
      cocSlider.disabled = dofCoc <= 0;
      cocVal.textContent = (dofCoc > 0 ? parseFloat(cocSlider.value) : 0).toFixed(1);
    };
    syncDofUi();
    dofBox.addEventListener('change', () => {
      dofCoc = dofBox.checked ? parseFloat(cocSlider.value) : 0;
      syncDofUi(); syncUrl();
      if (serverReady) { scheduleDrag(); scheduleIdle(); }
    });
    cocSlider.addEventListener('input', () => {
      if (dofBox.checked) { dofCoc = parseFloat(cocSlider.value); cocVal.textContent = dofCoc.toFixed(1); }
      if (serverReady) scheduleDrag();
    });
    cocSlider.addEventListener('change', () => { syncUrl(); if (serverReady) scheduleIdle(); });

    // ---- B2 画面（display-only：不重置累积，链完成后服务端 0-sample 再编码）----
    let dispTimer = null;
    function submitDisplay() {
      if (dispTimer) clearTimeout(dispTimer);
      dispTimer = setTimeout(() => {
        dispTimer = null;
        if (serverReady) submitRender(IDLE, { noCamera: true });
      }, 250);
    }
    const bloomBox = document.getElementById('re-in-bloom');
    const satSlider = document.getElementById('re-in-sat');
    const tintSlider = document.getElementById('re-in-tint');
    const satVal = document.getElementById('re-v-sat');
    const tintVal = document.getElementById('re-v-tint');
    satVal.textContent = (dispSat > 0 ? dispSat : 1).toFixed(2);
    tintVal.textContent = (dispTint > 0 ? dispTint : 1).toFixed(2);
    bloomBox.checked = dispBloom === 1;
    bloomBox.addEventListener('change', () => {
      dispBloom = bloomBox.checked ? 1 : 0;
      syncUrl(); submitDisplay();
    });
    satSlider.addEventListener('input', () => {
      satTouched = true;
      dispSat = parseFloat(satSlider.value);
      satVal.textContent = dispSat.toFixed(2);
      submitDisplay();
    });
    tintSlider.addEventListener('input', () => {
      tintTouched = true;
      dispTint = parseFloat(tintSlider.value);
      tintVal.textContent = dispTint.toFixed(2);
      submitDisplay();
    });

    // ---- B3 太阳（radiance 变化：释放才提交，服务端重置累积重染）----
    const sunAzSl = document.getElementById('re-in-sunaz');
    const sunElSl = document.getElementById('re-in-sunel');
    const sunIntSl = document.getElementById('re-in-sunint');
    const sunAzV = document.getElementById('re-v-sunaz');
    const sunElV = document.getElementById('re-v-sunel');
    const sunIntV = document.getElementById('re-v-sunint');
    const readSun = () => {
      sun = { az: parseFloat(sunAzSl.value), el: parseFloat(sunElSl.value),
              int: parseFloat(sunIntSl.value) };
      sunAzV.textContent = sun.az.toFixed(0) + '°';
      sunElV.textContent = sun.el.toFixed(0) + '°';
      sunIntV.textContent = sun.int.toFixed(0);
    };
    readSun();
    [sunAzSl, sunElSl, sunIntSl].forEach(sl => {
      sl.addEventListener('input', readSun);
      sl.addEventListener('change', () => {   // 释放即重染
        syncUrl();
        if (serverReady) submitRender(IDLE, { noCamera: true });
      });
    });

    // ---- B4 暂停/继续精修 ----
    const pauseBtn = mkBtn('pause', SVG.pause, '暂停/继续精修 (P)', () => {
      refinePaused = !refinePaused;
      pauseBtn.innerHTML = refinePaused ? SVG.play : SVG.pause;
      pauseBtn.title = refinePaused ? '继续精修 (P)' : '暂停精修 (P)';
      if (refinePaused) {
        // 取消在跑链；显示中的帧保留
        if (submittedRenderId)
          fetch(endpoint(authPath(`/api/renders/${submittedRenderId}/cancel`)),
                { method: 'POST' }).catch(() => {});
        show(model + ' 精修已暂停');
      } else {
        show(model + ' 继续精修');
        scheduleIdle();
      }
    });

    // ---- P1-6 实时统计面板（与设置面板互斥，同位右侧）----
    const statsPanel = document.createElement('div');
    statsPanel.className = 're-panel';
    statsPanel.hidden = true;
    statsPanel.innerHTML = '<h4>实时统计</h4>'
      + '<div class="re-row"><span>场景</span><b id="re-stat-model" style="margin-left:auto;font-weight:600"></b></div>'
      + '<div class="re-row"><span>预览分辨率</span><b id="re-stat-res" style="margin-left:auto;font-weight:600"></b></div>'
      + '<div class="re-row"><span>采样</span><b id="re-stat-spp" style="margin-left:auto;font-weight:600"></b></div>'
      + '<div class="re-row"><span>chunk 间隔</span><b id="re-stat-cad" style="margin-left:auto;font-weight:600"></b></div>'
      + '<div class="re-row"><span>曝光</span><b id="re-stat-exp" style="margin-left:auto;font-weight:600"></b></div>'
      + '<div class="re-row"><span>预计剩余</span><b id="re-stat-eta" style="margin-left:auto;font-weight:600"></b></div>';
    document.body.appendChild(statsPanel);
    const stat = (id, v) => { const el = document.getElementById(id); if (el && v != null) el.textContent = v; };
    function median(arr) {
      if (!arr.length) return null;
      const s = [...arr].sort((a, b) => a - b);
      return s[s.length >> 1];
    }
    // P2-12：剩余 chunk 数 × 间隔中位数（几何计划下用最近一次样本增量估 chunk）
    function etaSeconds() {
      if (lastBudget <= 0 || lastSample >= lastBudget) return 0;
      if (cadence.length < 2 || sampleHist.length < 2) return null;
      const n = sampleHist.length;
      const chunk = Math.max(1, sampleHist[n - 1] - sampleHist[n - 2]);
      const remainingChunks = Math.max(1, (lastBudget - lastSample) / chunk);
      return remainingChunks * median(cadence) / 1000;
    }
    function updateStats() {
      if (statsPanel.hidden) return;
      stat('re-stat-model', model);
      stat('re-stat-res', `${IDLE.width}×${IDLE.height}`);
      stat('re-stat-spp', lastBudget > 0 ? `${lastSample}/${lastBudget} spp` : '—');
      const m = median(cadence);
      stat('re-stat-cad', m != null ? `${Math.round(m)} ms` : '—');
      const e = tierExposure(camera.position);
      stat('re-stat-exp', e > 0 ? e.toFixed(1) : (autoOn ? '自动' : '—'));
      const eta = etaSeconds();
      stat('re-stat-eta', eta == null ? '—' : (eta <= 0 ? '完成' : `~${Math.max(1, Math.round(eta))} s`));
    }
    mkBtn('stats', '<svg viewBox="0 0 24 24"><line x1="5" y1="20" x2="5" y2="12"/><line x1="12" y1="20" x2="12" y2="6"/><line x1="19" y1="20" x2="19" y2="10"/></svg>',
          '实时统计', () => {
            statsPanel.hidden = !statsPanel.hidden;
            panel.hidden = true;
            if (!statsPanel.hidden) updateStats();
          });

    // ---- P2-13 首帧/当前帧 A/B 对比 ----
    const abImg = document.createElement('img');
    abImg.className = 're-ab'; abImg.alt = ''; abImg.hidden = true;
    document.body.appendChild(abImg);
    const abDiv = document.createElement('div');
    abDiv.className = 're-ab-div'; abDiv.hidden = true;
    document.body.appendChild(abDiv);
    const abTagA = document.createElement('div');
    abTagA.className = 're-ab-tag'; abTagA.style.left = '12px'; abTagA.hidden = true;
    const abTagB = document.createElement('div');
    abTagB.className = 're-ab-tag'; abTagB.style.right = '64px'; abTagB.hidden = true;
    document.body.appendChild(abTagA); document.body.appendChild(abTagB);
    function abApply() {
      overlay.style.clipPath = `inset(0 0 0 ${abPct}%)`;
      abDiv.style.left = `calc(${abPct}% - 1px)`;
    }
    function abShow() {
      if (!chainFirstUrl) return;
      abOn = true;
      abImg.src = chainFirstUrl;
      abImg.hidden = false; abDiv.hidden = false;
      abTagA.hidden = false; abTagB.hidden = false;
      abTagA.textContent = '首帧 ' + chainFirstSample + ' spp';
      abTagB.textContent = '当前 ' + lastSample + ' spp';
      abPct = 50; abApply();
    }
    function abHide() {
      abOn = false;
      overlay.style.clipPath = '';
      abImg.hidden = true; abDiv.hidden = true;
      abTagA.hidden = true; abTagB.hidden = true;
    }
    abDiv.addEventListener('pointerdown', (ev) => {
      ev.preventDefault();
      const move = (e) => {
        abPct = Math.min(98, Math.max(2, e.clientX / innerWidth * 100));
        abApply();
      };
      const up = () => {
        removeEventListener('pointermove', move);
        removeEventListener('pointerup', up);
      };
      addEventListener('pointermove', move);
      addEventListener('pointerup', up);
    });
    mkBtn('compare', '<svg viewBox="0 0 24 24"><rect x="4" y="5" width="16" height="14" rx="1"/><line x1="12" y1="5" x2="12" y2="19"/></svg>',
          '首帧/当前对比', () => { abOn ? abHide() : abShow(); });

    // ---- P1-7 场景切换器（设置面板内一行）----
    const SCENES = [
      { name: '内外景', id: 'bistro' },
      { name: '内景', id: 'bistro-interior' },
      { name: '测试球', id: 'sphere-test' }
    ];
    const sceneRow = panel.querySelector('.re-row');
    const sceneWrap = document.createElement('div');
    sceneWrap.innerHTML = '<h4 style="margin-top:12px">场景</h4><div class="re-q" id="re-scene"></div>';
    panel.insertBefore(sceneWrap, sceneRow);
    function syncSceneButtons() {
      document.querySelectorAll('#re-scene button')
        .forEach(b => b.classList.toggle('on', b.dataset.model === model));
    }
    SCENES.forEach(s => {
      const b = document.createElement('button');
      b.textContent = s.name; b.dataset.model = s.id;
      b.classList.toggle('on', s.id === model);
      b.addEventListener('click', () => switchScene(s.id));
      sceneWrap.lastElementChild.appendChild(b);
    });
    function switchScene(m) {
      if (m === model) return;
      model = m;
      serverReady = false;
      autoExposure = null;
      setBar(null);
      overlay.hidden = true;               // 旧场景帧不再代表新场景
      show('切换 ' + model);
      // 关旧链 + 关旧 WS（先摘 onclose 防止触发重连逻辑）
      if (sessionId) fetch(endpoint(authPath(`/api/sessions/${sessionId}/cancel`)),
                           { method: 'POST' }).catch(() => {});
      socketWanted = false;
      if (sock) { sock.onclose = null; try { sock.close(); } catch (_) {} sock = null; }
      syncUrl();
      syncSceneButtons();
      updateStats();
      createSession();
    }

    // ---- P1-10 URL 状态同步（分享当前画面设置）----
    function syncUrl() {
      params.set('model', model);
      params.set('idleRes', `${IDLE.width}x${IDLE.height}`);
      if (!autoOn && manualExp > 0) params.set('exposure', manualExp);
      else params.delete('exposure');
      // B1/B2/B3：相机、画面与太阳设置（画面为 display-only，太阳重染）
      if (fovTouched) params.set('fov', Math.round(camera.fov));
      else params.delete('fov');
      if (dofCoc > 0) params.set('coc', dofCoc.toFixed(1)); else params.delete('coc');
      if (satTouched) params.set('sat', dispSat); else params.delete('sat');
      if (tintTouched) params.set('tint', dispTint); else params.delete('tint');
      if (dispBloom >= 0) params.set('bloom', dispBloom ? '1' : '0'); else params.delete('bloom');
      if (sun && (sun.az !== 90 || sun.el !== 12 || sun.int !== 85))
        params.set('sun', `${Math.round(sun.az)},${Math.round(sun.el)},${Math.round(sun.int)}`);
      else params.delete('sun');
      history.replaceState(null, '', `${location.pathname}?${params}`);
    }

    // ---- P1-9 连接失败错误卡片 ----
    const errCard = document.createElement('div');
    errCard.className = 're-err';
    errCard.hidden = true;
    errCard.innerHTML = '<div class="card"><b>无法连接渲染服务端</b>'
      + '<span style="display:block;color:#94a3b8;font-size:12px;margin-top:6px">'
      + '正在自动重试。也可以在仓库根目录手动启动：</span>'
      + '<code>./demo.sh</code>'
      + '<button id="re-err-retry">立即重试</button></div>';
    document.body.appendChild(errCard);
    errCard.querySelector('#re-err-retry').addEventListener('click', () => {
      if (retryTimer) { clearTimeout(retryTimer); retryTimer = null; }
      createSession();
    });

    // 键盘：F 全屏 · R 回位 · 1-4 质量档 · H 隐藏 UI（不干扰输入框）
    window.addEventListener('keydown', (ev) => {
      if (ev.target && /INPUT|TEXTAREA/.test(ev.target.tagName)) return;
      const k = ev.key.toLowerCase();
      if (k === 'f') toggleFullscreen();
      else if (k === 'r') goHome();
      else if (k === 'h') document.body.classList.toggle('re-hideui');
      else if (k === 'p') document.getElementById('re-btn-pause').click();
      else if (k >= '1' && k <= '4') setQuality(parseInt(k, 10) - 1);
    });
    // 触控适配：粗指针下芯片放大、提示换触控文案
    if (window.matchMedia && matchMedia('(pointer: coarse)').matches)
      hint.textContent = '单指 环视 · 双指 缩放 · 停手自动精修';

    function endpoint(path) { return base + path; }
    function authPath(path) {
      if (!token) return path;
      return path + (path.includes('?') ? '&' : '?') + 'token=' + encodeURIComponent(token);
    }
    function websocketUrl() {
      const url = new URL(base);
      url.protocol = url.protocol === 'https:' ? 'wss:' : 'ws:';
      url.pathname = '/api/sessions/' + sessionId + '/events';
      if (token) url.search = 'token=' + encodeURIComponent(token);
      return url.toString();
    }
    function postJson(path, payload) {
      return fetch(endpoint(authPath(path)), {
        method: 'POST',
        headers: {'Content-Type': 'application/json'},
        body: JSON.stringify(payload || {})
      }).then((response) => response.json());
    }

    function cameraPayload() {
      const direction = controls.target.clone().sub(camera.position).normalize();
      // 现场排障：记录最近一次提交的相机与轨道目标（window.__remoteDebug）
      dbg.lastCamera = {
        position: [camera.position.x, camera.position.y, camera.position.z],
        target: [controls.target.x, controls.target.y, controls.target.z],
        fov: camera.fov
      };
      return {
        position: [camera.position.x, camera.position.y, camera.position.z],
        direction: [direction.x, direction.y, direction.z],
        up: [camera.up.x, camera.up.y, camera.up.z],
        fov: camera.fov,
        // B1 景深：对焦距离自动取轨道目标距离（所见即所得），coc 是光圈
        // 强度（px）；dofCoc=0 关闭（服务端 focus/coc 协议早已支持）
        focus: dofCoc > 0 ? camera.position.distanceTo(controls.target) : 0,
        coc: dofCoc
      };
    }

    // 相机先落库，成功后跟一帧对应档位的渲染（服务端 submitRender 读的是
    // session 当前相机）。
    // 现场排障计数器（控制台可直接读 window.__remoteDebug）
    const dbg = window.__remoteDebug = { dragFired: 0, idleFired: 0, submits: 0,
                                         skippedInFlight: 0, idleRetried: 0,
                                         sceneStages: [] };
    let interacting = false;

    let lastSubmitOk = false;
    // opts.noCamera：display-only / 太阳变更的重提交不带相机（服务端相机
    // 未变化本来也不会 bump generation，显式省略更干净）。
    function submitRender(tier, opts) {
      opts = opts || {};
      if (refinePaused && opts.noCamera) return;   // 暂停时不再自动精修
      if (!serverReady) return;   // P1：断线/场景切换期间不提交（防杂散请求）
      if (postInFlight) { dbg.skippedInFlight++; dirty = true; return; }
      dbg.submits++;
      postInFlight = true;
      // 批1：camera 并入 render 请求体（一次 RTT），服务端 applyCamera 语义
      // 与 POST /camera 相同（generation++、取消在跑链）。
      const payload = Object.assign({}, tier);
      if (!opts.noCamera) {
        payload.cameraGeneration = generation;
        payload.camera = cameraPayload();
      }
      const e = tierExposure(camera.position);
      if (e > 0) payload.exposure = e;
      // B2/B3：画面与太阳参数随每次提交幂等携带——服务端 display-only 不
      // 重置累积，太阳值不变也不重置；断线/切场景后自动恢复生效。
      if (satTouched) payload.saturationBoost = dispSat;
      if (tintTouched) payload.warmTint = dispTint;
      if (dispBloom >= 0) payload.bloom = dispBloom;
      if (sun) {
        payload.sunAz = sun.az; payload.sunEl = sun.el; payload.sunInt = sun.int;
      }
      dbg.lastExposure = e;
      // 自动档下滑条跟随当前有效曝光（手动时用户在拖，不打扰）
      if (autoOn) {
        expSlider.value = e;
        expLabel.textContent = e.toFixed(1);
      }
      postJson('/api/sessions/' + sessionId + '/render', payload)
        .then((result) => {
          lastSubmitOk = true;
          if (result.generation) generation = result.generation;
          if (result.renderId && result.renderId !== submittedRenderId) {
            submittedRenderId = result.renderId;
            chainFirstUrl = null;      // 新链：A/B 首帧基准重取
            if (abOn) abHide();
          }
        })
        .catch(() => { dirty = true; lastSubmitOk = false; })
        .finally(() => {
          postInFlight = false;
          // 批1：in-flight 期间被丢弃的拖动 tick 在请求完成后立即以最新相机
          // 补发（不再等下一个 change 事件）；失败的提交不链式重试（防 429 自旋）。
          if (dirty && serverReady && lastSubmitOk) {
            dirty = false;
            submitRender(interacting ? DRAG : IDLE);
          }
        });
    }

    // 拖动档：throttle 节流（设计 9：约 10-15Hz，服务端只保留最新 generation）。
    // change 事件在 damping 下有长尾，所以只认 start..end 区间内的 change。
    function scheduleDrag() {
      dirty = true;
      if (dragTimer) return;
      dragTimer = setTimeout(() => {
        dragTimer = null;
        if (!dirty) return;
        dirty = false;
        dbg.dragFired++;
        submitRender(DRAG);
      }, dragMs);
    }

    // 精修档：用户停手（'end'）后 idleMs 触发；被 in-flight 跳过时短暂重试，
    // 保证精修链最终一定被提交（debounce 语义落在 'end' 上，不受 damping
    // 尾随 change 的干扰）。重试有上限：慢网络下 in-flight 长期不返回时
    // 放弃本次（置回 dirty，下一次交互/refresh 会重新触发），避免无限转。
    const IDLE_RETRY_MAX = 5;
    let idleRetries = 0;
    function scheduleIdle() {
      if (idleTimer) clearTimeout(idleTimer);
      idleTimer = setTimeout(() => {
        idleTimer = null;
        if (refinePaused) { idleRetries = 0; return; }   // B4：暂停精修
        if (interacting) { idleRetries = 0; return; }
        if (postInFlight) {
          if (++idleRetries > IDLE_RETRY_MAX) { idleRetries = 0; return; }
          dbg.idleRetried++;
          scheduleIdle();
          return;
        }
        idleRetries = 0;
        dbg.idleFired++;
        dirty = false;
        submitRender(IDLE);
      }, idleMs);
    }

    controls.addEventListener('start', () => {
      interacting = true;
      if (idleTimer) { clearTimeout(idleTimer); idleTimer = null; }
      if (serverReady && !overlay.hidden && !abOn) overlay.style.filter = 'blur(4px)';
      if (serverReady) scheduleDrag();
    });
    controls.addEventListener('change', () => {
      if (serverReady && interacting) scheduleDrag();
    });
    controls.addEventListener('end', () => {
      interacting = false;
      if (!serverReady) return;
      scheduleDrag();
      scheduleIdle();
    });

    function onEvent(msg) {
      if (msg.type === 'session-ready') {
        serverReady = true;
        // 服务端 session 的相机是标定机位（preset）：先同步到本地轨道再提交，
        // 避免页面默认机位覆盖它（例如 Bistro 内景被推到墙外导致一片漆黑）。
        // 目标距离钳到 targetDist（默认 4m，房间尺度）：继承默认机位的 ~15m
        // 会让内景起步的 target 落在穿墙远处，一旋转轨道就整体扫出楼外。
        // 外景大范围浏览可传 &targetDist=12。相机 pos/朝向不变，首帧不变。
        fetch(endpoint(authPath('/api/sessions/' + sessionId)))
          .then((response) => response.json())
          .then((info) => {
            applySessionCamera(info);          // 同步轨道 + 缓存标定机位（R 回位用）
            // B1：FOV 默认跟随 session preset；?fov= / 用户改过则覆盖
            if (fovTouched) {
              camera.fov = urlFov;
              camera.updateProjectionMatrix();
            }
            fovSlider.value = Math.round(camera.fov);
            fovVal.textContent = Math.round(camera.fov) + '°';
            // B2/B3：画面与太阳初值从 session 同步（仅当 URL 未覆盖）
            if (!satTouched && info.saturationBoost > 0) {
              satSlider.value = info.saturationBoost;
              satVal.textContent = info.saturationBoost.toFixed(2);
            }
            if (!tintTouched && info.warmTint > 0) {
              tintSlider.value = info.warmTint;
              tintVal.textContent = info.warmTint.toFixed(2);
            }
            if (!sun && info.sun && info.sun.has) {
              sunAzSl.value = info.sun.az; sunElSl.value = info.sun.el;
              sunIntSl.value = info.sun.int;
              readSun();
            }
            // 室内自动提亮档参数（server 从 preset 下发；indoorExposure 0 = 无）
            autoExposure = {
              base: info.exposure || 1.0,
              indoor: info.indoorExposure || 0,
              min: info.indoorMin || [0, 0, 0],
              max: info.indoorMax || [0, 0, 0],
              margin: info.indoorMargin || 1.5
            };
            // 自动档下把滑条/读数同步到当前基准曝光
            if (autoOn) {
              expSlider.value = autoExposure.base;
              expLabel.textContent = autoExposure.base.toFixed(1);
            }
            setBar(0);
            show(model + ' ready');
            scheduleDrag();
            scheduleIdle();
          })
          .catch(() => { setBar(0); show(model + ' ready'); scheduleDrag(); scheduleIdle(); });
        return;
      }
      if (msg.type === 'scene-progress') {
        // P2-11：真实加载进度（阶段权重映射，单调不降——合并场景
        // parse/decode 各跑两轮）
        const w = LOAD_STAGES[msg.stage] || [0, 1];
        const inner = msg.total > 0 ? Math.min(1, msg.done / msg.total) : 1;
        loadFrac = Math.max(loadFrac, w[0] + (w[1] - w[0]) * inner);
        if (dbg.sceneStages.indexOf(msg.stage) < 0) dbg.sceneStages.push(msg.stage);
        setBar(loadFrac);
        show(model + ' 加载中 · ' + msg.stage +
             (msg.total > 0 ? ' ' + msg.done + '/' + msg.total :
              (msg.done > 0 ? ' ' + msg.done : '')));
        return;
      }
      if (msg.type === 'progress' || msg.type === 'preview-ready') {
        // 旧帧丢弃：只显示当前提交链、当前 generation、更大 sequence 的帧
        if (msg.renderId !== submittedRenderId) return;
        if (msg.generation < generation) return;
        if (msg.renderId === shownRenderId && msg.sequence <= shownSequence) return;
        if (!msg.previewUrl) return;
        // P1 统计：chunk 间隔（预览事件到达时刻）+ 采样计数
        const nowT = performance.now();
        if (lastPrevT) { cadence.push(nowT - lastPrevT); if (cadence.length > 8) cadence.shift(); }
        lastPrevT = nowT;
        lastSample = msg.sample; lastBudget = msg.sampleBudget;
        sampleHist.push(msg.sample); if (sampleHist.length > 8) sampleHist.shift();
        if (!chainFirstUrl) { chainFirstUrl = new URL(authPath(msg.previewUrl), base).toString(); chainFirstSample = msg.sample; }
        // B5：fetch→blob→objectURL 单次取图解码（此前 probe+overlay 对同一
        // URL 取图/解码两次）；解码完成后再上屏，晚到的旧帧按 sequence 丢弃。
        const url = new URL(authPath(msg.previewUrl), base).toString();
        const seq = msg.sequence, rid = msg.renderId;
        fetch(url)
          .then((r) => { if (!r.ok) throw new Error(r.status); return r.blob(); })
          .then((blob) => {
            if (rid !== submittedRenderId ||
                (rid === shownRenderId && seq <= shownSequence)) return;
            const objUrl = URL.createObjectURL(blob);
            const probe = new Image();
            probe.onload = () => {
              if (rid !== submittedRenderId || seq <= shownSequence) {
                URL.revokeObjectURL(objUrl);
                return;
              }
              if (overlayObjUrl) URL.revokeObjectURL(overlayObjUrl);
              overlayObjUrl = objUrl;
              overlay.src = objUrl;
              overlay.hidden = false;
              shownRenderId = rid;
              shownSequence = seq;
              applyBlur(msg.sample, msg.sampleBudget);
              setBar(msg.sampleBudget > 0 ? msg.sample / msg.sampleBudget : null);
              const eta = etaSeconds();
              show(model + ' spp ' + msg.sample + '/' + msg.sampleBudget +
                   (eta > 0 ? ' · ~' + Math.max(1, Math.round(eta)) + 's' : ''));
              updateStats();
              if (abOn) abTagB.textContent = '当前 ' + msg.sample + ' spp';
            };
            probe.onerror = () => URL.revokeObjectURL(objUrl);
            probe.src = objUrl;
          })
          .catch(() => {});
        return;
      }
      if (msg.type === 'render-completed') {
        setBar(1);
        show(model + ' done spp ' + msg.sample);
        return;
      }
      if (msg.type === 'render-canceled') {
        show(model + ' canceled — moving?');
        return;
      }
      if (msg.type === 'server-error') {
        show('error: ' + (msg.message || 'server error'));
      }
    }

    function connect() {
      if (!socketWanted || !sessionId) return;
      sock = new WebSocket(websocketUrl());
      sock.onopen = () => {
        connectAttempts = 0;
        show(serverReady ? 'connected' : 'loading ' + model);
      };
      sock.onmessage = (e) => {
        try { onEvent(JSON.parse(e.data)); } catch (_) { /* 非 JSON 帧忽略 */ }
      };
      sock.onclose = () => {
        if (!socketWanted) return;
        show('reconnecting');
        // 服务端重启等导致 session 丢失时，重建 session 再连。重建前先取消
        // 旧 session 的在跑渲染链，避免其空跑到 TTL 白耗 GPU（若服务端已
        // 重启则该请求 404，忽略即可）。
        if (++connectAttempts >= 3) {
          connectAttempts = 0;
          serverReady = false;
          if (sessionId) {
            fetch(endpoint(authPath('/api/sessions/' + sessionId + '/cancel')),
                  { method: 'POST' }).catch(() => {});
          }
          createSession();
          return;
        }
        setTimeout(connect, Math.min(1000 * connectAttempts, 5000));
      };
      sock.onerror = () => { try { sock.close(); } catch (_) {} };
    }

    let retryTimer = null;
    function createSession() {
      shownRenderId = null;
      shownSequence = 0;
      submittedRenderId = null;
      serverReady = false;
      loadFrac = 0;
      sampleHist.length = 0;
      cadence.length = 0;
      chainFirstUrl = null;
      abHide();
      setBar(null);
      errCard.hidden = true;
      postJson('/api/sessions', { model: model })
        .then((result) => {
          if (!result.sessionId) { errCard.hidden = false; show('rejected: ' + (result.error || '?')); return; }
          sessionId = result.sessionId;
          generation = 0;
          socketWanted = true;
          errCard.hidden = true;
          show('loading ' + model);
          connect();
        })
        .catch(() => {
          // P1-9：连接失败 → 居中错误卡片（保持 2s 自动重试，卡片可手动触发）
          show('unavailable, retrying');
          errCard.hidden = false;
          if (retryTimer) clearTimeout(retryTimer);
          retryTimer = setTimeout(createSession, 2000);
        });
    }

    createSession();
    return { refresh: () => { if (serverReady) { scheduleDrag(); scheduleIdle(); } } };
  }

  window.addEventListener('DOMContentLoaded', App.boot);

})(window.App);
