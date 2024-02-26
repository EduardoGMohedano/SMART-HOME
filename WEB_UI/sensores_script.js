var temperatureGauge;
var humidityGauge; 
var updateTime = 2000;

document.addEventListener("DOMContentLoaded", function(event) {
  temperatureGauge = new JustGage({
      id: "temperatureGauge",
      value: 24, // Sample value, replace with actual data
      min: 0,
      max: 50,
      title: "Temperatura",
      label: "Temperatura °C",
      valueMinFontSize : 30,
      labelMinFontSize : 18,
    });

  humidityGauge = new JustGage({
      id: "humidityGauge",
      value: 40, // Sample value, replace with actual data
      min: 0,
      max: 100,
      title: "Humedad",
      label: "Humedad %",
      valueMinFontSize : 30,
      labelMinFontSize : 18,
  });

});

//Function to request temperature data
function updateGauge(){
  fetch('http://192.168.3.44/sensor?type=temp')
    .then( response => {
      return response.text()
    })
    .then(data => {
      temperatureGauge.refresh(data);
    })
    .catch(error => console.error('Error while fetching data: ',error));
}

//Function to request humidity data
function updateGaugeH(){
  fetch('http://192.168.3.44/sensor?type=humi')
    .then( response => {
      return response.text()
    })
    .then(data => {
      humidityGauge.refresh(data);
    })
    .catch(error => console.error('Error while fetching data: ',error));
}

setInterval(updateGauge, updateTime);
setInterval(updateGaugeH,updateTime);