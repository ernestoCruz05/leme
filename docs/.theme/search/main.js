(() => {
  const field = document.getElementById('mkdocs-search-query');
  const status = document.getElementById('search-status');
  const results = document.getElementById('search-results');
  const query = new URLSearchParams(location.search).get('q')?.trim() || '';
  field.value = query;
  if (!query) return;
  if (query.length < 2 || query.length > 200) {
    status.textContent = 'Use between 2 and 200 characters.';
    return;
  }
  if (!/[\p{L}\p{N}]/u.test(query)) {
    status.textContent = `No results for "${query}". Try a word or a setting name.`;
    return;
  }
  status.textContent = `Searching for "${query}"...`;
  let worker;
  let timeout;
  const fail = () => {
    clearTimeout(timeout);
    worker?.terminate();
    status.textContent = 'Search could not load. Reload the page to try again, or browse the documentation sections.';
  };
  try {
    worker = new Worker(`${results.dataset.baseUrl}/search/worker.js`);
    timeout = setTimeout(fail, 30000);
    worker.onerror = fail;
    worker.onmessage = event => {
      if (event.data.allowSearch) {
        worker.postMessage({ query: query.replace(/[\\:+\-~^*]/g, '\\$&') });
      } else if (event.data.results) {
        clearTimeout(timeout);
        const groups = new Map();
        for (const match of event.data.results) {
          const path = match.location.split('#')[0];
          if (!groups.has(path)) groups.set(path, []);
          groups.get(path).push(match);
        }
        status.textContent = groups.size
          ? `${groups.size} ${groups.size === 1 ? 'result' : 'results'} for "${query}".`
          : `No results for "${query}". Try a shorter word or a setting name.`;
        const terms = query.toLowerCase().split(/\s+/);
        const pattern = new RegExp(terms.map(term => term.replace(/[.*+?^${}()|[\]\\]/g, '\\$&')).join('|'), 'gi');
        for (const [location, matches] of groups) {
          const best = matches[0];
          const page = matches.find(match => match.location === location) || best;
          const article = document.createElement('article');
          const path = document.createElement('p');
          path.className = 'result-path';
          path.textContent = location || 'Documentation overview';
          const heading = document.createElement('h2');
          const link = document.createElement('a');
          link.href = `${results.dataset.baseUrl}/${location}`;
          link.textContent = page.title;
          heading.append(link);
          const positions = terms.map(term => best.text.toLowerCase().indexOf(term)).filter(position => position >= 0);
          let start = positions.length ? Math.max(0, Math.min(...positions) - 60) : 0;
          start = Math.max(0, best.text.lastIndexOf(' ', start) + 1);
          const limit = start + 260;
          const boundary = best.text.lastIndexOf(' ', limit);
          const end = best.text.length > limit && boundary > start ? boundary : limit;
          const excerpt = `${start ? '…' : ''}${best.text.slice(start, end)}${end < best.text.length ? '…' : ''}`;
          const summary = document.createElement('p');
          let offset = 0;
          for (const match of excerpt.matchAll(pattern)) {
            summary.append(excerpt.slice(offset, match.index));
            const mark = document.createElement('mark');
            mark.textContent = match[0];
            summary.append(mark);
            offset = match.index + match[0].length;
          }
          summary.append(excerpt.slice(offset));
          const sections = document.createElement('ul');
          sections.className = 'search-sections';
          for (const match of matches) {
            if (!match.location.includes('#') || match.title === page.title) continue;
            const item = document.createElement('li');
            const section = document.createElement('a');
            section.href = `${results.dataset.baseUrl}/${match.location}`;
            section.textContent = match.title;
            item.append(section);
            sections.append(item);
          }
          article.append(path, heading, summary);
          if (sections.childElementCount) article.append(sections);
          results.append(article);
        }
        worker.terminate();
      }
    };
    worker.postMessage({ init: true });
  } catch {
    fail();
  }
})();
