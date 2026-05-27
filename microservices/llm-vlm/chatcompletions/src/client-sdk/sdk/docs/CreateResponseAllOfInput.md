# CreateResponseAllOfInput

Text, image, or file inputs to the model, used to generate a response.  Learn more: - [Text inputs and outputs](/docs/guides/text) - [Image inputs](/docs/guides/images) - [File inputs](/docs/guides/pdf-files) - [Conversation state](/docs/guides/conversation-state) - [Function calling](/docs/guides/function-calling) 

## Properties

Name | Type | Description | Notes
------------ | ------------- | ------------- | -------------

## Example

```python
from openapi_client.models.create_response_all_of_input import CreateResponseAllOfInput

# TODO update the JSON string below
json = "{}"
# create an instance of CreateResponseAllOfInput from a JSON string
create_response_all_of_input_instance = CreateResponseAllOfInput.from_json(json)
# print the JSON string representation of the object
print(CreateResponseAllOfInput.to_json())

# convert the object into a dict
create_response_all_of_input_dict = create_response_all_of_input_instance.to_dict()
# create an instance of CreateResponseAllOfInput from a dict
create_response_all_of_input_from_dict = CreateResponseAllOfInput.from_dict(create_response_all_of_input_dict)
```
[[Back to Model list]](../README.md#documentation-for-models) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to README]](../README.md)


