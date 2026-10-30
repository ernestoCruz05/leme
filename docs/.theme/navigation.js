const narrow = matchMedia('(max-width: 760px)');
const navigation = document.getElementById('navigation');
navigation.open = !narrow.matches;
narrow.addEventListener('change', event => { navigation.open = !event.matches; });
