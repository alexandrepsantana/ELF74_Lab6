#include "system_tm4c1294.h" // CMSIS-Core
#include "driverleds.h"      // device drivers
#include "driverbuttons.h"   // device drivers
#include "cmsis_os2.h"       // CMSIS-RTOS

// Defines.
#define FOREVER          1
#define FALSE            0
#define TRUE             1
#define NUMBEROFLEDS     4
#define PERIOD           100  // 10.0 ms
#define DUTYCYCLESTEP    10   // 1.0 ms
#define BUTTONPRESSED    1700 // 170.0 ms
#define NUMBEROFMESSAGES 8

// Tipos do usuário.
typedef struct {
  osMessageQueueId_t q;
  osMutexId_t        mutex;
  uint8_t            ledNumber;
  uint8_t            dutyCycle;
  uint8_t            isSetup;
} msg_t;

// Variáveis globais.
osMessageQueueId_t btnQ;
osMessageQueueId_t ledQs[NUMBEROFLEDS];

// Protótipos de funções.
void GPIOJ_Handler(void);
void controlTask(void*);
void ledTask(void*);
void softPWM(osMutexId_t, uint8_t, uint8_t);

// Função main(). Ponto de entrada do programa.
void main(void) {
  SystemInit();

  // Inicializar periféricos.
  LEDInit(LED4 | LED3 | LED2 | LED1);
  ButtonInit(USW2 | USW1);
  ButtonIntEnable(USW2 | USW1);

  // Inicializar núcleo do SO.
  osKernelInitialize();

  // Inicializar tarefas.
  osThreadNew(controlTask, NULL, NULL);
  
  if (osKernelGetState() == osKernelReady) {
    osKernelStart();
  }

  // Nunca chega neste ponto.
  while (FOREVER);
}

// Tarefa de controle chamada pela função main().
void controlTask(void *arg) {
  uint8_t     setup;
  msg_t       mLed1, mLed2, mLed3, mLed4;
  msg_t*      msgs[NUMBEROFLEDS] = { &mLed1, &mLed2, &mLed3, &mLed4 };
  osMutexId_t portFMutex, portNMutex;

  // Inicializar filas.
  for (int i = 0; i < NUMBEROFLEDS; ++i)
    ledQs[i] = osMessageQueueNew(NUMBEROFMESSAGES, sizeof(msg_t), NULL);
  btnQ = osMessageQueueNew(NUMBEROFMESSAGES, sizeof(uint8_t), NULL);

  // Inicializar muteces.
  portNMutex = osMutexNew(NULL);
  portFMutex = osMutexNew(NULL);
  
  // Inicializar estados.
  for (int i = 0; i < NUMBEROFLEDS; ++i) {
    // LED1 = 0b0001 = 1, LED2 = 0b0010 = 2, LED3 = 0b0100 = 4, LED4 = 0b1000 = 8.
    msgs[i]->ledNumber = (1 << i);
    // LEDs em duty cycle de 10%.
    msgs[i]->dutyCycle = DUTYCYCLESTEP;
    // LED1 em estado de ajuste; os demais, não.
    msgs[i]->isSetup = i == 0 ? TRUE : FALSE;
    // Guardar a indicação da fila dentro da mensagem inicial do LED.
    msgs[i]->q = ledQs[i];
    // Se o número do LED for ou LED1, ou LED2, então usa o mutex da PORTN;
    // senão, usa o da PORTF.
    msgs[i]->mutex = i < 2 ? portNMutex : portFMutex;
    // Escrever mensagem para inicializar LEDs.
    osMessageQueuePut(ledQs[i], (void*)msgs[i], 0, 0);
    // Iniciar threads dos LEDs com uma mensagem inicial, para que o código
    // da tarefa do LED, que é agnóstica ao número do LED, tenha
    // algum valor como base (no caso, msg_t::q).
    osThreadNew(ledTask, (void*)msgs[i], NULL);
  }

  setup = 0; // LED1 em estado de ajuste.
  
  while (FOREVER) {
    uint8_t btnMsg = 0, newDutyCycle = 0;
    
    // Habilita interrupção dos botões.
    ButtonIntEnable(USW2 | USW1);
    
    // Verificar fila do botão.
    if (osOK == osMessageQueueGet(btnQ, (void*)&btnMsg, NULL, 0)) {
      // Botão enviou mensagem!
      
      // A mensagem que chega da ISR é a leitura dos botões USW1 e USW2,
      // cujos bits estarão em 0 em caso de pressionamento. Portanto,
      // vamos negar a mensagem e, em seguida, pegar os bits 0 (USW1)
      // e 1 (USW2).
      switch (~btnMsg & (USW2 | USW1)) {
        // Se o bit 0 estiver setado, então apertaram o botão USW1!
        case USW1:
          // Tirar o LED em ajuste do modo de ajuste.
          msgs[setup]->isSetup = FALSE;
          // Escrever mensagem na fila, para que o LED "saiba" que saiu
          // do modo de ajuste.
          osMessageQueuePut(ledQs[setup], (void*)msgs[setup], 0, 0);
          // Passar para o próximo LED da sequência.
          setup = (setup + 1) % NUMBEROFLEDS;
          // Por LED em modo de ajuste, ao menos na mensagem.
          msgs[setup]->isSetup = TRUE;
          // Escrever a mensagem na fila do novo LED, para que ele "saiba"
          // que está em modo de ajuste.
          osMessageQueuePut(ledQs[setup], (void*)msgs[setup], 0, 0);
          break;
          
        // Se o bit 1 estiver setado, então apertaram o botão USW2!
        case USW2:
          // Calcular o novo duty cycle do LED atualmente em ajuste.
          newDutyCycle = msgs[setup]->dutyCycle + DUTYCYCLESTEP;
          // Se o novo duty cycle for maior do que PERIOD, zerar o valor.
          if (newDutyCycle > PERIOD) newDutyCycle = 0;
          // Salva o novo duty cycle na mensagem que será enviada.
          msgs[setup]->dutyCycle = newDutyCycle;
          // Escrever mensagem na fila do LED atual, em estado de ajuste,
          // para que a luminosidade do LED seja afetada.
          osMessageQueuePut(ledQs[setup], (void*)msgs[setup], 0, 0);
          break;
          
        // Se os dois botões/nenhum botão estiver(em) apertado(s), ignorar
        // a entrada.
        default:
          // Não fazer nada.
          break;
      }
    }
  }
}

// Tarefa dos LEDs chamada pela função controlTask().
void ledTask(void *arg) {
  // Mensagem passada como argumento da função, para que a tarefa
  // "saiba" de qual LED dentre os quatro LEDs ela controla.
  msg_t* msg = (msg_t*)arg;
  // Obtém a fila (state.q) que corresponde ao LED desta tarefa,
  // o duty cycle inicial, o número do LED e o mutex do LED.
  msg_t state = *msg;
  
  while (FOREVER) {
    // Verificar fila do LED. Se contiver mensagem, guardá-la na
    // variável state.
    if (osOK == osMessageQueueGet(state.q, (void*)msg, NULL, 0)) 
      state = *msg;

    if (!state.isSetup) {
      // Se não for modo de ajuste, ficar somente no soft PWM.
      softPWM(state.mutex, state.ledNumber, state.dutyCycle);
    } else {
      // Se for ajuste, entrar em modo de ajuste.
      for (int i = 0; i < 8 /* 8 x 10.0 ms */; ++i)
        // Soft PWM por 80 ms no LED da tarefa, sob um certo duty cycle.
        softPWM(state.mutex, state.ledNumber, state.dutyCycle);
      // Apagar LED por 80 ms.
      osDelay(800 /* 80.0 ms */);
    }
  }
}

// Soft PWM chamada pela função ledTask().
void softPWM(osMutexId_t mutex, uint8_t ledNumber, uint8_t dutyCycle) {
  // Se o duty cycle for zero, não acende o LED.
  if (dutyCycle != 0) {
    // Bloco protegido por mutex.
    osMutexAcquire(mutex, osWaitForever);
    LEDOn(ledNumber);
    osMutexRelease(mutex);
    // Aguarda o tempo do duty cycle com o LED aceso.
    osDelay(dutyCycle);
  }
  
  // Se o duty cycle for 100%, não apaga o LED.
  if (dutyCycle != PERIOD) {
    // Bloco protegido por mutex.
    osMutexAcquire(mutex, osWaitForever);
    LEDOff(ledNumber);
    osMutexRelease(mutex);
    // Aguarda o resto do período com o LED apagado.
    osDelay(PERIOD - dutyCycle);
  }
}

// ISR dos botões. Chamada por pressionamento dos botões USW1 ou USW2.
void GPIOJ_Handler(void) {
  static uint32_t debouncer = 0;
  uint8_t btnMsg;
  
  // Desabilitar a interrupção.
  ButtonIntDisable(USW2 | USW1);
  // Limpar a reserva da interrupção.
  ButtonIntClear(USW2 | USW1);

  // Impede que pulsos espúrios do botão também executem a ISR.
  if (osKernelGetTickCount() - debouncer > BUTTONPRESSED) {
    // Botão foi pressionado!
    // Qual botão foi pressionado? Ajustar mensagem a ser enviada.
    btnMsg = ButtonRead(USW2 | USW1);
    // Escrever mensagem na fila do botão, que se comunica com
    // a tarefa de controle.
    osMessageQueuePut(btnQ, (void*)&btnMsg, 0, 0);
  }
  
  // Ajusta o filtro passa-baixa do botão.
  debouncer = osKernelGetTickCount();
}
