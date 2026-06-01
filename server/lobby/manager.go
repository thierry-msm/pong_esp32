package lobby

import (
	"encoding/json"
	"fmt"
	"net"
	"net/http"
	"sync"
	"time"

	"pong-server/game"

	"github.com/gorilla/websocket"
)

// Prazos limites para detecção de conexões fantasmas
const (
	writeWait      = 500 * time.Millisecond // Tempo maximo para tentar escrever uma mensagem
	pongWait       = 10 * time.Second       // Tempo limite esperando a resposta do Pong
	pingPeriod     = 4 * time.Second        // Frequencia de envio de pings (deve ser menor que pongWait)
	stateQueueSize = 1                      // Mantem apenas o frame mais recente por cliente
)

var upgrader = websocket.Upgrader{
	CheckOrigin: func(r *http.Request) bool {
		return true // Permite qualquer origem para facilidade de testes online
	},
}

// Manager coordena as conexões de rede ativas e integra com a máquina de estados do jogo
type Manager struct {
	mu          sync.Mutex
	PlayerLeft  *Client
	PlayerRight *Client
	Engine      *game.Engine
}

type Client struct {
	conn *websocket.Conn
	side string
	send chan []byte
}

// NewManager cria e configura um novo gerenciador de lobby
func NewManager(engine *game.Engine) *Manager {
	return &Manager{
		Engine: engine,
	}
}

type SetupMessage struct {
	Type string `json:"type"`
	Side string `json:"side"`
}

type ClientMessage struct {
	Type string  `json:"type"`
	Dir  float64 `json:"dir"`
}

// HandleWS processa o handshake inicial de novas conexões
func (m *Manager) HandleWS(w http.ResponseWriter, r *http.Request) {
	fmt.Printf("[WS CONNECT] Nova conexao HTTP recebida de %s. Iniciando Handshake...\n", r.RemoteAddr)

	conn, err := upgrader.Upgrade(w, r, nil)
	if err != nil {
		fmt.Printf("[WS CONNECT] Falha ao fazer o upgrade da conexao de %s: %v\n", r.RemoteAddr, err)
		return
	}
	if tcpConn, ok := conn.UnderlyingConn().(*net.TCPConn); ok {
		_ = tcpConn.SetNoDelay(true)
	}

	// Define políticas de segurança e monitoramento de queda
	conn.SetReadLimit(512)
	conn.SetReadDeadline(time.Now().Add(pongWait))

	// Sempre que receber um Pong, renova a data de tolerância
	conn.SetPongHandler(func(string) error {
		conn.SetReadDeadline(time.Now().Add(pongWait))
		return nil
	})

	m.mu.Lock()
	var assignedSide string
	var client *Client

	// Tenta alocar vaga livre
	if m.PlayerLeft == nil {
		assignedSide = "left"
		client = &Client{conn: conn, side: assignedSide, send: make(chan []byte, stateQueueSize)}
		m.PlayerLeft = client
		fmt.Printf("[LOBBY ASSIGN] Cliente %s registrado com sucesso na vaga [ESQUERDA].\n", r.RemoteAddr)
	} else if m.PlayerRight == nil {
		assignedSide = "right"
		client = &Client{conn: conn, side: assignedSide, send: make(chan []byte, stateQueueSize)}
		m.PlayerRight = client
		fmt.Printf("[LOBBY ASSIGN] Cliente %s registrado com sucesso na vaga [DIREITA].\n", r.RemoteAddr)
	} else {
		m.mu.Unlock()
		fmt.Printf("[LOBBY REJECT] Conexao recusada para %s: Ambas as vagas ja estao ocupadas.\n", r.RemoteAddr)
		errPayload, _ := json.Marshal(map[string]string{
			"type":  "error",
			"error": "lobby_full",
		})
		conn.WriteMessage(websocket.TextMessage, errPayload)
		conn.Close()
		return
	}

	// Calcula total de jogadores ativos e notifica a engine
	count := 0
	if m.PlayerLeft != nil {
		count++
	}
	if m.PlayerRight != nil {
		count++
	}
	m.Engine.PlayerConnected(count)
	m.mu.Unlock()

	// Envia mensagem de boas-vindas com o lado do jogador
	setupMsg := SetupMessage{
		Type: "setup",
		Side: assignedSide,
	}
	if payload, err := json.Marshal(setupMsg); err == nil {
		conn.SetWriteDeadline(time.Now().Add(writeWait))
		if err := conn.WriteMessage(websocket.TextMessage, payload); err != nil {
			fmt.Printf("[WS SETUP ERROR] Falha ao enviar setup para [%s]: %v\n", assignedSide, err)
			m.disconnect(client, "falha no envio do setup")
			return
		}
	}

	// Inicializa a escuta assíncrona do cliente
	go m.writeLoop(client)
	go m.readLoop(client)
}

// readLoop mantém a escuta de comandos ativos do jogador
func (m *Manager) readLoop(client *Client) {
	// A desconexão é centralizada no defer por segurança
	defer m.disconnect(client, "fim do loop de rede/leitura")

	for {
		_, payload, err := client.conn.ReadMessage()
		if err != nil {
			// Erro na leitura indica desconexão legítima ou estouro de deadline
			break
		}

		// A cada mensagem lida com sucesso, renovamos a tolerância de conexão
		client.conn.SetReadDeadline(time.Now().Add(pongWait))

		// Processa mensagens recebidas
		var msg ClientMessage
		if err := json.Unmarshal(payload, &msg); err == nil {
			if msg.Type == "input" {
				// Repassa movimento de raquete para a Engine
				m.Engine.SetPaddleDir(client.side, msg.Dir)
			} else if msg.Type == "ready" {
				// Repassa clique de lobby para a Engine
				m.Engine.ToggleReady(client.side)
			}
		} else {
			fmt.Printf("[WS RECV ERROR] Falha ao desserializar JSON de [%s]: %s\n", client.side, string(payload))
		}
	}
}

// writeLoop serializa todas as escritas de um cliente e evita bloquear o loop fisico.
func (m *Manager) writeLoop(client *Client) {
	pingTicker := time.NewTicker(pingPeriod)
	defer pingTicker.Stop()

	for {
		select {
		case payload := <-client.send:
			client.conn.SetWriteDeadline(time.Now().Add(writeWait))
			if err := client.conn.WriteMessage(websocket.TextMessage, payload); err != nil {
				fmt.Printf("[WS WRITE ERROR] Falha ao transmitir estado para [%s]: %v\n", client.side, err)
				m.disconnect(client, "erro na transmissao de dados (Write)")
				return
			}
		case <-pingTicker.C:
			client.conn.SetWriteDeadline(time.Now().Add(writeWait))
			if err := client.conn.WriteMessage(websocket.PingMessage, nil); err != nil {
				fmt.Printf("[WS PING ERROR] Falha ao enviar Ping para [%s]. Expurgando conexao zumbi.\n", client.side)
				m.disconnect(client, "falha no envio de Ping (Heartbeat)")
				return
			}
		}
	}
}

func (m *Manager) disconnect(client *Client, reason string) {
	if client == nil {
		return
	}

	m.mu.Lock()
	defer m.mu.Unlock()

	// Garante que só limpará a vaga se a conexão sendo limpa ainda for a ativa daquele lado
	if client.side == "left" && m.PlayerLeft == client {
		m.PlayerLeft = nil
		client.conn.Close()
		fmt.Printf("[WS DISCONNECT] Vaga [ESQUERDA] liberada. Motivo: %s\n", reason)
		m.Engine.PlayerDisconnected()
	} else if client.side == "right" && m.PlayerRight == client {
		m.PlayerRight = nil
		client.conn.Close()
		fmt.Printf("[WS DISCONNECT] Vaga [DIREITA] liberada. Motivo: %s\n", reason)
		m.Engine.PlayerDisconnected()
	}
}

// StartGameLoop roda a 30 FPS computando as regras e atualizando ambos os clientes em tempo real
func (m *Manager) StartGameLoop() {
	ticker := time.NewTicker(33 * time.Millisecond) // ~30 FPS
	defer ticker.Stop()

	for range ticker.C {
		// 1. Atualiza regras físicas e estados na engine
		m.Engine.Update()

		// 2. Captura os clientes ativos sem segurar o lock durante o envio.
		m.mu.Lock()
		left := m.PlayerLeft
		right := m.PlayerRight
		m.mu.Unlock()

		// 3. Enfileira o mesmo frame para ambos sem bloquear o tick fisico.
		if left != nil || right != nil {
			payload, err := m.Engine.GetStateJSON()
			if err == nil {
				m.enqueueLatest(left, payload)
				m.enqueueLatest(right, payload)
			}
		}
	}
}

func (m *Manager) enqueueLatest(client *Client, payload []byte) {
	if client == nil {
		return
	}

	select {
	case client.send <- payload:
		return
	default:
	}

	select {
	case <-client.send:
	default:
	}

	select {
	case client.send <- payload:
	default:
		fmt.Printf("[WS DROP] Cliente [%s] nao consumiu o frame anterior; mantendo apenas o estado mais recente.\n", client.side)
	}
}
