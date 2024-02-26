document.getElementById('loginForm').addEventListener('submit', function(event) {
    event.preventDefault(); // Prevent the form from submitting the traditional way

    var username = document.getElementById('username').value;
    var password = document.getElementById('password').value;
    var hashedpasword = CryptoJS.MD5(password).toString();
    var errorMessage = document.getElementById('error-message');

    let auth_data = {
        'user': username,
        'pass': hashedpasword
    };

    fetch('http://192.168.3.44/authentication',{
        method: 'POST',
        mode: 'no-cors', //temporary setting to get data from server board
        headers: {'Content-Type': 'application/json'},
        body : JSON.stringify(auth_data),
    })
    .then( response => response.json())
    .catch(error => console.error('Error while fetching data: ',error));


    //how to authenticate this with the status of the http response?????
    if(username === 'admin' && password === 'password') {
        // Success: In a real application, you might redirect or show a success message
        alert('Login successful!');
        window.location.href = 'home.html'
    } else {
        // Error: Show an error message
        errorMessage.innerText = 'Invalid username or password';
        errorMessage.style.color = 'red';
    }
});
