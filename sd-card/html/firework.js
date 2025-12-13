/**
 * firework.launch(message, type, durationMs)
 *
 * Minimal, dependency-free notification widget for the local UI.
 * Kept API-compatible with the previous jQuery-based implementation.
 */
(function (window) {
  "use strict";

  function asString(value) {
    return typeof value === "string" ? value : String(value ?? "");
  }

  function getTopOffsetPx() {
    var offset = 10;
    var nodes = document.querySelectorAll(".firework");
    for (var i = 0; i < nodes.length; i++) {
      offset += nodes[i].offsetHeight + 10;
    }
    return offset;
  }

  function removeNode(node) {
    if (!node) return;
    node.style.opacity = "0";
    window.setTimeout(function () {
      if (node && node.parentNode) node.parentNode.removeChild(node);
    }, 250);
  }

  window.firework = {
    launch: function (message, type, durationMs) {
      if (typeof message !== "string") {
        console.error("firework.launch called without a string message");
        message = asString(message);
      }

      var element = document.createElement("div");
      element.className = "firework" + (type ? " " + type : "");
      element.style.top = getTopOffsetPx() + "px";
      element.style.opacity = "0";

      var close = document.createElement("a");
      close.setAttribute("role", "button");
      close.setAttribute("tabindex", "0");
      close.style.cssText = "float:right; padding:0 6px; font-size:20px; line-height:20px; cursor:pointer;";
      close.textContent = "×";
      close.onclick = function () { removeNode(element); };
      close.onkeydown = function (e) {
        if (e.key === "Enter" || e.key === " ") removeNode(element);
      };

      var text = document.createElement("span");
      text.innerHTML = message;

      element.appendChild(close);
      element.appendChild(text);
      document.body.appendChild(element);

      // Fade in
      window.setTimeout(function () { element.style.opacity = "1"; }, 0);

      var delay = typeof durationMs === "number" ? durationMs : 1500;
      window.setTimeout(function () { removeNode(element); }, delay);
    },

    remove: function (selectorOrNode) {
      if (typeof selectorOrNode === "string") {
        removeNode(document.querySelector(selectorOrNode));
      } else {
        removeNode(selectorOrNode);
      }
    },
  };
})(window);
