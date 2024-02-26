// Fetch the menu HTML and include it in the page
const headerTexts = {
    'home.html': 'Home Page',
    'sensores.html': 'Sensores',
    'iluminacion.html': 'Iluminacion',
    // Add more mappings as needed
};

function updateHeader(headerText){
    document.getElementById('nav-header').innerText = headerText;
}

fetch('./menu.html')
    .then(response => response.text())
    .then(data => {
        document.getElementById('menu-container').innerHTML = data;
        
        const currentPage = window.location.pathname.split('/').pop(); // Gets the current file name
        const navHeader = headerTexts[currentPage] || 'ESP32 Web server'; // Default header if file name not found
        return navHeader;
    })
    .then(navHeader => {
        updateHeader(navHeader);
    });