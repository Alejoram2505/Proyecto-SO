# Chat SO

Proyecto base en C para Linux, ajustado al protocolo compartido de `ChatPacket` de 1024 bytes.

## Compilación

```bash
make
```

## Ejecución

Servidor:

```bash
./servidor <puerto>
```

Cliente:

```bash
./cliente <username> <IP_servidor> <puerto>
```

## Comandos del cliente

```text
/broadcast <mensaje>
/msg <usuario> <mensaje>
/status <ACTIVE|BUSY|INACTIVE>
/list
/info <usuario>
/help
/exit
```

## Notas

- `protocolo.h` se copió respetando el orden y tamaños del protocolo.
- El servidor usa `pthread` para atender clientes concurrentemente.
- Se valida nombre e IP únicos en el registro.
- Se soporta desconexión controlada y abrupta.
- La lista de usuarios se mantiene solo en memoria.
