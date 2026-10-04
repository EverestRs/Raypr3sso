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
    // 展示默认：合并内外景（?model= 可切 bistro-interior / sphere-test）
    const model = params.get('model') || 'bistro';
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
    const DRAG = { width: 320, height: 180, sppBudget: 8, maxDepth: 4, chunkSpp: 8, neeK: 1 };
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
    function tierExposure(pos) {
      if (exposure > 0) return exposure;                 // 手动覆盖优先
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
    let connectAttempts = 0;
    // 旧帧丢弃（设计 8）：renderId + generation + sequence 三元组
    let submittedRenderId = null;
    let shownRenderId = null;
    let shownSequence = 0;
    // 相机提交状态机
    let dragTimer = null;
    let idleTimer = null;
    let postInFlight = false;
    let dirty = false;

    const overlay = document.createElement('img');
    overlay.alt = '';
    overlay.hidden = true;
    overlay.style.cssText = 'position:fixed;inset:0;width:100%;height:100%;object-fit:contain;pointer-events:none;z-index:10;';
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
      .re-bar.done { opacity:0; transition:opacity .8s; }`;
    document.head.appendChild(style);

    const title = document.createElement('div');
    title.className = 're-chip re-title';
    title.innerHTML = 'RenderEngine <small>CUDA path tracing · remote progressive viewer</small>';
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
        focus: 0,
        coc: 0
      };
    }

    // 相机先落库，成功后跟一帧对应档位的渲染（服务端 submitRender 读的是
    // session 当前相机）。
    // 现场排障计数器（控制台可直接读 window.__remoteDebug）
    const dbg = window.__remoteDebug = { dragFired: 0, idleFired: 0, submits: 0,
                                         skippedInFlight: 0, idleRetried: 0 };
    let interacting = false;

    let lastSubmitOk = false;
    function submitRender(tier) {
      if (postInFlight) { dbg.skippedInFlight++; dirty = true; return; }
      dbg.submits++;
      postInFlight = true;
      // 批1：camera 并入 render 请求体（一次 RTT），服务端 applyCamera 语义
      // 与 POST /camera 相同（generation++、取消在跑链）。
      const payload = Object.assign({ cameraGeneration: generation,
                                      camera: cameraPayload() }, tier);
      const e = tierExposure(camera.position);
      if (e > 0) payload.exposure = e;
      dbg.lastExposure = e;
      postJson('/api/sessions/' + sessionId + '/render', payload)
        .then((result) => {
          lastSubmitOk = true;
          if (result.generation) generation = result.generation;
          if (result.renderId) submittedRenderId = result.renderId;
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
            // 室内自动提亮档参数（server 从 preset 下发；indoorExposure 0 = 无）
            autoExposure = {
              base: info.exposure || 1.0,
              indoor: info.indoorExposure || 0,
              min: info.indoorMin || [0, 0, 0],
              max: info.indoorMax || [0, 0, 0],
              margin: info.indoorMargin || 1.5
            };
            setBar(0);
            show(model + ' ready');
            scheduleDrag();
            scheduleIdle();
          })
          .catch(() => { setBar(0); show(model + ' ready'); scheduleDrag(); scheduleIdle(); });
        return;
      }
      if (msg.type === 'progress' || msg.type === 'preview-ready') {
        // 旧帧丢弃：只显示当前提交链、当前 generation、更大 sequence 的帧
        if (msg.renderId !== submittedRenderId) return;
        if (msg.generation < generation) return;
        if (msg.renderId === shownRenderId && msg.sequence <= shownSequence) return;
        if (!msg.previewUrl) return;
        // 批1：预解码后再上屏（img.decode）——高频 JPEG 换帧不再出现
        // 半解码闪烁；晚到的旧解码被 sequence 检查丢弃。
        const url = new URL(authPath(msg.previewUrl), base).toString();
        const seq = msg.sequence, rid = msg.renderId;
        const probe = new Image();
        probe.onload = () => {
          if (rid === submittedRenderId && seq <= shownSequence) return;
          overlay.src = url;
          overlay.hidden = false;
          shownRenderId = rid;
          shownSequence = seq;
          setBar(msg.sampleBudget > 0 ? msg.sample / msg.sampleBudget : null);
          show(model + ' spp ' + msg.sample + '/' + msg.sampleBudget);
        };
        probe.onerror = () => {};
        probe.src = url;
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
      const socket = new WebSocket(websocketUrl());
      socket.onopen = () => {
        connectAttempts = 0;
        show(serverReady ? 'connected' : 'loading ' + model);
      };
      socket.onmessage = (e) => {
        try { onEvent(JSON.parse(e.data)); } catch (_) { /* 非 JSON 帧忽略 */ }
      };
      socket.onclose = () => {
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
      socket.onerror = () => { try { socket.close(); } catch (_) {} };
    }

    function createSession() {
      shownRenderId = null;
      shownSequence = 0;
      submittedRenderId = null;
      serverReady = false;
      postJson('/api/sessions', { model: model })
        .then((result) => {
          if (!result.sessionId) { show('rejected: ' + (result.error || '?')); return; }
          sessionId = result.sessionId;
          generation = 0;
          socketWanted = true;
          show('loading ' + model);
          connect();
        })
        .catch(() => {
          show('unavailable, retrying');
          setTimeout(createSession, 2000);
        });
    }

    createSession();
    return { refresh: () => { if (serverReady) { scheduleDrag(); scheduleIdle(); } } };
  }

  window.addEventListener('DOMContentLoaded', App.boot);

})(window.App);
