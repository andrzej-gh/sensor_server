#include <stdio.h>
#include <stdlib.h>
#include <json-c/json.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <arpa/inet.h>

#define SEC_IN_USECS 1000000
#define DEF_PORT 5200

typedef struct Sensor {
   int  id;
   char sensorType[32];
   int  minValue;
   int  maxValue;
   char encoderType[32];
   int  frequency;
} Sensor;

typedef struct Client {
   struct Client *next;
   int fd;
   struct sockaddr_in addr;
} Client;

typedef struct Simulator {
   /* Server Data */
   int                servFd;
   struct sockaddr_in servAddr;
   /* Sensor Data */
   Sensor sensor;
   /* Connected clients data */
   Client *first;
} Simulator;

typedef enum Quality {
 NORMAL,
 WARNING,
 ALARM,
} Quality;


char *
QualityToString(Quality q)
{
   switch (q) {
   case NORMAL:
      return "Normal";
   case WARNING:
      return "Warning";
   case ALARM:
      return "Alarm";
   default:
      return "Unknown";
   }
}


/* List functions */


void
listFree(Client *client)
{
  if (client == NULL) {
     return;
  }

  listFree(client->next);
  free(client);
}


void
listRemoveItem(Client *first, Client *client)
{
  Client *ptr = first;
  Client *prev = NULL;

  while (ptr != NULL) {
    prev = ptr;
    ptr = ptr->next;
    if (ptr == client) {
       prev->next = ptr->next;
       free(ptr);
       return;
    }
  }
}


void
listAdd(Client **first, Client *client)
{
   Client *ptr;

   if (*first == NULL) {
      *first = client;
   } else {
      ptr = *first;
      while (ptr->next != NULL) {
         ptr = ptr->next;
      }
      ptr->next = client;
   }
}


/* Sensor functions */


int
Sensor_getSleepTime(Sensor *sensor)
{
   return SEC_IN_USECS / sensor->frequency;
}


Quality
Sensor_getQuality(Sensor *sensor, int value)
{
   int range = sensor->maxValue - sensor->minValue;
   int valInRange = value - sensor->minValue;
   int proc = (100 * valInRange) / range;

   if (proc <= 10 || proc >= 90) {
      return ALARM;
   } else if (proc <= 25 || proc >= 75) {
      return WARNING;
   }

   return NORMAL;
}


int
Sensor_getRandValue(Sensor *sensor)
{
   int range = sensor->maxValue - sensor->minValue;

   return rand() % range + sensor->minValue;
}


void
Sensor_getSensorData(Sensor *sensor, struct json_object *jSensor)
{
   /* TODO: Add data validation */
   struct json_object *jSensorParam;
   /* ID */
   jSensorParam = json_object_object_get(jSensor, "ID");
   sensor->id = json_object_get_int(jSensorParam);
   /* TYpe */
   jSensorParam = json_object_object_get(jSensor, "Type");
   strncpy(sensor->sensorType, json_object_get_string(jSensorParam), 31);
   /* MinValue */
   jSensorParam = json_object_object_get(jSensor, "MinValue");
   sensor->minValue = json_object_get_int(jSensorParam);
   /* MaxValue */
   jSensorParam = json_object_object_get(jSensor, "MaxValue");
   sensor->maxValue = json_object_get_int(jSensorParam);
   /* EncoderType */
   jSensorParam = json_object_object_get(jSensor, "EncoderType");
   strncpy(sensor->encoderType, json_object_get_string(jSensorParam), 31);
   /* Frequency */
   jSensorParam = json_object_object_get(jSensor, "Frequency");
   sensor->frequency = json_object_get_int(jSensorParam);
}


/* Thread workers */


void *
server(void *data)
{
   Simulator *simulator = (Simulator *) data;
   int newClientFd;
   struct sockaddr_in newClientAddr;
   unsigned int newClientAddrLen = sizeof(newClientAddr);
   Client *newClient;

   simulator->servFd = socket(AF_INET, SOCK_STREAM, 0);
   memset(&simulator->servAddr, 0, sizeof(simulator->servAddr));
   simulator->servAddr.sin_family = AF_INET;
   simulator->servAddr.sin_addr.s_addr = htonl(INADDR_ANY);
   simulator->servAddr.sin_port = htons(DEF_PORT + simulator->sensor.id);

   bind(simulator->servFd, (struct sockaddr*)&simulator->servAddr,
        sizeof(simulator->servAddr));

   listen(simulator->servFd, 8);

   printf("Sensor ID=%d is waiting for recievers...\n", simulator->sensor.id);
   fflush(stdout);

   while (1) {
      newClientFd = accept(simulator->servFd, (struct sockaddr*)&newClientAddr,
                           &newClientAddrLen);
      printf("Got new connection for Sensor ID=%d!\n", simulator->sensor.id);
      fflush(stdout);
      newClient = malloc(sizeof(Client));
      newClient->fd = newClientFd;
      newClient->addr = newClientAddr;
      newClient->next = NULL;
      listAdd(&simulator->first, newClient);
   }

   /* Probably locking mechanism should be  provieded for list */
   listFree(simulator->first);

   return NULL;
}


void *
simulator(void *data)
{
   Simulator *simulator = (Simulator *) data;
   Sensor *sensor = &simulator->sensor;
   Client *ptr;
   int value;
   Quality q;
   char msg[255];

   while (1) {
      value = Sensor_getRandValue(sensor);
      q = Sensor_getQuality(sensor, value);
      sprintf(msg, "$FIX, %d, %s, %d, %s*", sensor->id, sensor->sensorType,
              value, QualityToString(q));
      ptr = simulator->first;
      while (ptr != NULL) {
         //printf("Send: %s\n", msg);
         write(ptr->fd, msg, strlen(msg));
         ptr = ptr->next;
      }
      usleep(Sensor_getSleepTime(sensor));
   }

   return NULL;
}


int main()
{
   struct json_object *jSensorConfig, *jSensors, *jSensor;
   static const char configFile[] = "sensorConfig.json";
   int len, i;
   pthread_t *simThreads;
   pthread_t *servThreads;
   Simulator *simulators;

   srand(time(NULL));

   /* Parse json file */
   jSensorConfig = json_object_from_file(configFile);
   jSensors = json_object_object_get(jSensorConfig, "Sensors");
   len = json_object_array_length(jSensors);
   simulators = malloc(len * sizeof(Simulator));
   for (i = 0; i < len; i++) {
      memset(&simulators[i], 0, sizeof(Simulator));
      jSensor = json_object_array_get_idx(jSensors, i);
      Sensor_getSensorData(&simulators[i].sensor, jSensor);
   }

   /* simulator threads to send random msgs */
   simThreads = malloc(len * sizeof(pthread_t));
   for (i = 0; i < len; i++) {
      pthread_create(&simThreads[i], NULL, simulator, &simulators[i]);
   }

   /* server threads to accept new connections */
   servThreads = malloc(len * sizeof(pthread_t));
   for (i = 0; i < len; i++) {
      pthread_create(&servThreads[i], NULL, server, &simulators[i]);
   }

   /* Join all threads */
   for(i = 0; i < len; i++) {
      pthread_join(simThreads[i], NULL);
      pthread_join(servThreads[i], NULL);
   }

   free(simulators);
   free(simThreads);
   free(servThreads);

   return 0;
}
