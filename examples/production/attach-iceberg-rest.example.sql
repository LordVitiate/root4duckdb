INSTALL iceberg;
LOAD iceberg;

-- Replace all placeholders outside version control.
CREATE SECRET iceberg_secret (
    TYPE iceberg,
    TOKEN 'REPLACE_ME'
);
ATTACH 'warehouse' AS compass_iceberg (
    TYPE iceberg,
    SECRET iceberg_secret,
    ENDPOINT 'https://REPLACE_ME'
);
