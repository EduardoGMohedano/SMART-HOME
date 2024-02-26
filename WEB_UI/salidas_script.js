//Deberia haber una variable asociada por cada output
let bulb_state = false;

function toggleLight(bulb) {
    bulb_state = !bulb_state;
    console.log("The bulb state is ", bulb_state);
    //fetch('http://192.168.3.44/output?type=temp')
    fetch('http://192.168.3.44/output',{
        method: 'POST',
        mode: 'no-cors', //temporary setting to get data from server board
        headers: {'Content-Type': 'text/html'},
        body : bulb_state ? '1' : '0',
    })
    .then( response => {
      return response.text()
    })
    .then(data => {
        console.log("Success sending POST request");
    })
    .catch(error => console.error('Error while fetching data: ',error));

    bulb.classList.toggle("light-bulb-on");
    bulb.classList.toggle("light-bulb-off");
}
