// SPDX-License-Identifier: AGPL-3.0-or-later
// The web setup page works without this. It adds two comforts: the number
// beside a slider follows it, and Delete and Remove ask first.
document.querySelectorAll('input[type=range]').forEach(function (i) {
  var o = i.parentNode.querySelector('output');
  if (o) i.addEventListener('input', function () { o.textContent = i.value; });
});
document.querySelectorAll('[data-ask]').forEach(function (b) {
  b.addEventListener('click', function (e) { if (!confirm(b.getAttribute('data-ask'))) e.preventDefault(); });
});
